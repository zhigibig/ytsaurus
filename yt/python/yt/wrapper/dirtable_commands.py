import functools
import hashlib
import itertools
import multiprocessing as mp
import os
import re

from .client import YtClient
from .config import get_config
from .errors import YtError
from .ypath import TablePath, ypath_dirname

import yt.yson as yson


def get_chunk_table_attributes():
    res = {}
    res["schema"] = [
        {"name": "filename", "type": "string", "sort_order": "ascending"},
        {"name": "part_index", "type": "int64", "sort_order": "ascending"},
        {"name": "offset", "type": "int64"},
        {"name": "filesize", "type": "int64"},
        {"name": "data", "type": "string"}
    ]
    res["optimize_for"] = "lookup"
    return res


def get_skynet_table_attributes():
    res = {}
    res["schema"] = [
        {"group": "meta", "name": "filename", "type": "string", "sort_order": "ascending"},
        {"group": "meta", "name": "part_index", "type": "int64", "sort_order": "ascending"},
        {"group": "meta", "name": "offset", "type": "int64"},
        {"group": "meta", "name": "filesize", "type": "int64"},
        {"group": "meta", "name": "data_size", "type": "int64"},
        {"group": "meta", "name": "sha1", "type": "string"},
        {"group": "meta", "name": "md5", "type": "string"},
        {"group": "data", "name": "data", "type": "string"}
    ]
    res["enable_skynet_sharing"] = True
    res["optimize_for"] = "scan"
    return res


class ChunkToUpload:
    def __init__(self, full_path, yt_name, part_index, data_size, offset, file_size):
        self.full_path = full_path
        self.yt_name = yt_name
        self.part_index = part_index
        self.data_size = data_size
        self.offset = offset
        self.file_size = file_size

    def make_row(self, for_sky_share):
        with open(self.full_path, "rb") as f:
            f.seek(self.offset)
            data = f.read(self.data_size)
            res = {
                "filename": self.yt_name,
                "part_index": self.part_index,
                "data": data,
                "offset": self.offset,
                "filesize": self.file_size
            }
            if for_sky_share:  # Documentation says that it"s not necessary, but without that it won"t work with big files (YT-18990)
                res["md5"] = hashlib.md5(data).digest()
                res["sha1"] = hashlib.sha1(data).digest()
                res["data_size"] = self.data_size
        return res


def get_files_to_upload(directory, recursive):
    for root, dirs, files in os.walk(directory):
        for file in files:
            full_path = os.path.join(root, file)
            yt_name = os.path.relpath(full_path, directory)
            yield (full_path, yt_name)

        if not recursive:
            break


def get_chunks(full_path, yt_name, part_size):
    file_size = os.path.getsize(full_path)
    offset = 0
    part_index = 0
    while offset < file_size:
        end = min(offset + part_size, file_size)
        yield ChunkToUpload(full_path=full_path, yt_name=yt_name, part_index=part_index, data_size=end - offset, offset=offset, file_size=file_size)
        offset = end
        part_index += 1


def get_chunks_to_upload(directory, recursive, part_size):
    res = []
    for full_path, yt_name in get_files_to_upload(directory, recursive):
        res.extend(get_chunks(full_path, yt_name, part_size))
    return res


def split_range(begin, end, parts_count):
    count = end - begin
    parts_count = max(parts_count, 1)
    d, r = divmod(count, parts_count)
    res = []
    ind = 0
    while begin < end:
        cur_end = begin + d + (1 if ind < r else 0)
        res.append((begin, cur_end))
        begin = cur_end
        ind += 1
    return res


def split_chunks(chunks, parts_count):
    return [chunks[begin:end] for begin, end in split_range(0, len(chunks), parts_count)]


def write_chunks(chunks, folder, client_config, transaction_id, for_sky_share):
    client = YtClient(config=client_config)
    with client.Transaction(transaction_id=transaction_id):
        table_path = client.find_free_subpath(folder)
        attributes = get_skynet_table_attributes() if for_sky_share else get_chunk_table_attributes()
        client.create("table", table_path, attributes=attributes)
        client.write_table(table_path, (c.make_row(for_sky_share=for_sky_share) for c in chunks), raw=False)
    return table_path


def get_file_sizes_from_table(client, yt_table):
    row_count = client.get_attribute(yt_table, "row_count")
    part_size = client.get_attribute(yt_table, "part_size")
    cur_row = 0
    res = {}
    while cur_row < row_count:
        t = TablePath(yt_table, start_index=cur_row, end_index=cur_row + 1, client=client)
        row, = client.read_table(t, raw=False)  # There is single row
        assert row["part_index"] == 0
        filename = row["filename"]
        filesize = row["filesize"]
        res[filename] = filesize
        row_count_for_file = filesize // part_size + bool(filesize % part_size)
        cur_row += row_count_for_file
    return res


def write_meta_file(client, yt_table, file_sizes, force):
    meta_file_path = yt_table + ".dirtable.meta"
    meta = yson.to_yson_type({
        "file_sizes": file_sizes
    })
    client.write_file(meta_file_path, yson.dumps(meta), force_create=True)


def get_file_sizes(client, yt_table):
    meta_file_path = yt_table + ".dirtable.meta"
    try:
        meta = yson.loads(client.read_file(meta_file_path).read())
        return meta["file_sizes"]
    except YtError:
        print("Warning: {} not found, try to get file sizes from table".format(meta_file_path))
        return get_file_sizes_from_table(client, yt_table)


def download_table(table_path, directory, client_config, transaction_id):
    client = YtClient(config=client_config)
    with client.Transaction(transaction_id=transaction_id):
        files = {}
        try:
            for row in client.read_table(table_path, raw=False):
                if row["filename"] not in files:
                    files[row["filename"]] = open(os.path.join(directory, row["filename"]), "r+b")
                f = files[row["filename"]]
                f.seek(row["offset"])
                f.write(yson.get_bytes(row["data"]))
        except Exception as e:
            raise e
        finally:
            for name, f in files.items():
                f.close()


def upload_directory_to_yt(directory, recursive, yt_table, part_size, process_count, force, prepare_for_sky_share):
    client = YtClient(config=get_config(None))
    chunks = get_chunks_to_upload(directory, recursive, part_size)
    chunks.sort(key=lambda c: (c.yt_name, c.part_index))
    file_sizes = {}
    for c in chunks:
        file_sizes[c.yt_name] = c.offset + c.data_size

    with client.Transaction(attributes={"title": "dirtable upload"}, ping=True) as tx:
        folder = ypath_dirname(yt_table)
        client.mkdir(folder, recursive=True)
        if not folder.endswith("/"):
            folder = folder + "/"

        attributes = get_skynet_table_attributes() if prepare_for_sky_share else get_chunk_table_attributes()
        client.create("table", yt_table, attributes=attributes, force=force)

        chunks = split_chunks(chunks, process_count)

        worker = functools.partial(
            write_chunks,
            folder=folder,
            client_config=get_config(client),
            transaction_id=tx.transaction_id,
            for_sky_share=prepare_for_sky_share)
        temp_tables = []

        pool = mp.Pool(process_count)
        try:
            temp_tables = [t for t in pool.map(worker, chunks)]
            pool.close()
        except:  # noqa
            pool.terminate()
            raise
        finally:
            pool.join()

        client.concatenate(temp_tables, yt_table)

        for t in temp_tables:
            client.remove(t)
        client.set_attribute(yt_table, "part_size", part_size)
        write_meta_file(client, yt_table, file_sizes, force)


def check_file_name(file_name, exact_filenames, filter_by_regexp, exclude_by_regexp):
    if exact_filenames and (file_name not in exact_filenames):
        return False
    if filter_by_regexp and (not filter_by_regexp.match(file_name)):
        return False
    if exclude_by_regexp and (exclude_by_regexp.match(file_name)):
        return False
    return True


def download_directory_from_yt(directory, yt_table, process_count, exact_filenames, filter_by_regexp, exclude_by_regexp):
    client = YtClient(config=get_config(None))
    if not os.path.exists(directory):
        os.makedirs(directory)

    with client.Transaction(attributes={"title": "dirtable download"}, ping=True) as tx:
        file_sizes = get_file_sizes(client, yt_table)
        filtered = False
        if exact_filenames or filter_by_regexp or exclude_by_regexp:
            filtered = True
            file_sizes = {key: value for key, value in file_sizes.items() if check_file_name(key, exact_filenames, filter_by_regexp, exclude_by_regexp)}
            if not file_sizes:
                print("Warning: there is no appropriate files")
        try:
            for filename, size in file_sizes.items():
                full_path = os.path.join(directory, filename)
                if not os.path.exists(os.path.dirname(full_path)):
                    os.makedirs(os.path.dirname(full_path))
                with open(full_path, "wb") as f:
                    f.write(b"\0" * size)
            row_count = client.get_attribute(yt_table, "row_count")
            tables = []
            if filtered:
                tables = [TablePath(yt_table, exact_key=name, client=client) for name in file_sizes]
            else:
                tables = [TablePath(yt_table, start_index=begin, end_index=end, client=client) for begin, end in split_range(0, row_count, process_count)]
            worker = functools.partial(download_table, directory=directory, client_config=get_config(client), transaction_id=tx.transaction_id)
            pool = mp.Pool(process_count)
            try:
                pool.map(worker, tables)
                pool.close()
            except:  # noqa
                pool.terminate()
                raise
            finally:
                pool.join()

        except Exception:
            for filename in file_sizes:
                try:
                    os.remove(os.path.join(directory, filename))
                except OSError:
                    pass
            raise


def list_files_from_yt(yt_table):
    client = YtClient(config=get_config(None))
    file_sizes = get_file_sizes(client, yt_table)
    for filename in file_sizes:
        print(filename)


def append_single_file(yt_table, fs_path, yt_name, process_count):
    assert os.path.isfile(fs_path), "{} must be existing file".format(fs_path)
    client = YtClient(config=get_config(None))
    with client.Transaction(attributes={"title": "dirtable append"}, ping=True) as tx:
        file_sizes = get_file_sizes(client, yt_table)

        assert yt_name not in file_sizes, "File {} already presents in {}".format(yt_name, yt_table)
        file_sizes[yt_name] = os.path.getsize(fs_path)
        part_size = client.get_attribute(yt_table, "part_size")

        file_chunks = list(get_chunks(fs_path, yt_name, part_size))

        folder = ypath_dirname(yt_table)
        if not folder.endswith("/"):
            folder = folder + "/"

        prefix_table = TablePath(yt_table, upper_key=yt_name, client=client)
        suffix_table = TablePath(yt_table, lower_key=yt_name, client=client)

        file_chunks = split_chunks(file_chunks, process_count)
        for_sky_share = client.get_attribute(yt_table, "enable_skynet_sharing", default=False)
        worker = functools.partial(write_chunks, folder=folder, client_config=get_config(client), transaction_id=tx.transaction_id, for_sky_share=for_sky_share)
        middle_tables = []

        pool = mp.Pool(process_count)
        try:
            middle_tables = [t for t in pool.map(worker, file_chunks)]
            pool.close()
        except:  # noqa
            pool.terminate()
            raise
        finally:
            pool.join()

        temp_concat_table = client.find_free_subpath(folder)
        attributes = get_skynet_table_attributes() if for_sky_share else get_chunk_table_attributes()
        client.create("table", temp_concat_table, attributes=attributes)
        client.run_merge(list(itertools.chain([prefix_table], middle_tables, [suffix_table])), temp_concat_table, mode="sorted")

        for t in middle_tables:
            client.remove(t)
        client.move(temp_concat_table, yt_table, force=True)
        client.set_attribute(yt_table, "part_size", part_size)
        write_meta_file(client, yt_table, file_sizes, force=True)


def add_upload_parser(parsers):
    parser = parsers.add_parser("upload", help="Upload directory to YT")
    parser.set_defaults(func=upload_directory_to_yt)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--part-size", type=int, default=4 * 1024 * 1024)

    parser.set_defaults(recursive=True)
    parser.add_argument("--recursive", action="store_true")
    parser.add_argument("--no-recursive", dest="recursive", action="store_false")
    parser.add_argument("--yt-table", required=True)
    parser.add_argument("--process-count", type=int, default=4)

    parser.set_defaults(force=False)
    parser.add_argument("--force", action="store_true")

    parser.set_defaults(prepare_for_sky_share=False)
    parser.add_argument("--prepare-for-sky-share", action="store_true")


def add_download_parser(parsers):
    parser = parsers.add_parser("download", help="Download directory from YT")
    parser.set_defaults(func=download_directory_from_yt)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--yt-table", required=True)
    parser.add_argument("--process-count", type=int, default=4)
    parser.add_argument("--exact-filenames", type=lambda s: s.split(","), help="Files to extract (separated by comma)")
    parser.add_argument("--filter-by-regexp", type=lambda s: re.compile(s), help="Files with name matching that regexp will be extracted")
    parser.add_argument("--exclude-by-regexp", type=lambda s: re.compile(s), help="Files with name matching that regexp will not be extracted")


def add_list_files_parser(parsers):
    parser = parsers.add_parser("list-files", help="List files from YT")
    parser.set_defaults(func=list_files_from_yt)
    parser.add_argument("--yt-table", required=True)


def add_append_single_file(parsers):
    parser = parsers.add_parser("append-single-file", help="Append single file to table")
    parser.set_defaults(func=append_single_file)
    parser.add_argument("--yt-table", required=True)
    parser.add_argument("--yt-name", required=True)
    parser.add_argument("--fs-path", required=True)
    parser.add_argument("--process-count", type=int, default=4)


def add_dirtable_parsers(subparsers):
    add_upload_parser(subparsers)
    add_download_parser(subparsers)
    add_list_files_parser(subparsers)
    add_append_single_file(subparsers)
