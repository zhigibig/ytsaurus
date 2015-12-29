#!/usr/bin/env python

import yt.wrapper as yt
import yt.logger as logger

import re
from argparse import ArgumentParser

yt.config.PREFIX = "//sys/scheduler/"

def get_event_log_tables():
    pattern = re.compile("event_log(\.\d*)?")
    return [str(obj)
            for obj in yt.list(yt.config.PREFIX[:-1], attributes=["type"])
            if obj.attributes.get("type") == "table" 
                and pattern.match(str(obj))]

def get_archive_number(table):
    if table == "event_log":
        return 0
    else:
        return int(table.split(".")[1])

def get_next_table(table):
    if table == "event_log":
        return "event_log.1"
    else:
        return "event_log." + str(int(table.split(".")[1]) + 1)

def get_prev_table(table):
    num = int(table.split(".")[1]) - 1
    if num == 0:
        return "event_log"
    else:
        return "event_log." + str(num)

def get_size(table):
    return yt.get(table + "/@resource_usage/disk_space")

def get_archive_disk_space_ratio():
    compression_ratio = yt.get("event_log/@compression_ratio")
    try:
        archive_compression_ratio = yt.get("event_log.1/@compression_ratio")
    except yt.YtResponseError as err:
        if err.is_resolve_error():
            archive_compression_ratio = 0.1
        else:
            raise
    return archive_compression_ratio / compression_ratio

def get_archive_compression_ratio():
    try:
        return yt.get("event_log.1/@compression_ratio")
    except yt.YtResponseError as err:
        if err.is_resolve_error():
            return 0.05
        raise
    
def get_possible_size_to_archive():
    attributes = yt.get("event_log/@")
    if attributes["row_count"] == 0:
        return 0
    return (attributes["processed_row_count"] * attributes["resource_usage"]["disk_space"]) / attributes["row_count"]

def get_desired_row_count_to_archive(desired_archive_size):
    free_archive_size = desired_archive_size
    if yt.exists("event_log.1"):
        free_archive_size -= yt.get("event_log.1/@resource_usage/disk_space")
    archive_ratio = get_archive_disk_space_ratio()
    size_to_archive = min(get_possible_size_to_archive(), free_archive_size / archive_ratio)
    
    attributes = yt.get("event_log/@")
    if attributes["row_count"] == 0:
        return 0
    return int(min(attributes["processed_row_count"], (attributes["row_count"] * size_to_archive) / attributes["resource_usage"]["disk_space"]))

def fennel_exists():
    try:
        yt.get("event_log/@processed_row_count")
        return True
    except yt.YtResponseError as err:
        if err.is_resolve_error():
            return False
        raise

def get_archived_row_count():
    try:
        return yt.get("event_log/@archived_row_count")
    except yt.YtResponseError as err:
        if err.is_resolve_error():
            return 0
        raise

def erase_archived_prefix():
    with yt.Transaction():
        archived_row_count = get_archived_row_count()
        if archived_row_count == 0:
            return

        logger.info("Erasing archived prefix of event log table")

        processed_row_count = yt.get("event_log/@processed_row_count")

        assert archived_row_count <= processed_row_count 
        yt.lock("event_log", mode="exclusive")
        yt.run_erase(yt.TablePath("event_log", start_index=0, end_index=archived_row_count))
        yt.set("event_log/@archived_row_count", 0)
        yt.set("event_log/@processed_row_count", processed_row_count - archived_row_count)

def rotate_archives(archive_size_limit, min_portion_to_archive):
    if yt.exists("event_log.1") and get_size("event_log.1") >= archive_size_limit - min_portion_to_archive:
        logger.info("Rotate event log archives")
        tables = get_event_log_tables()
        tables.sort(key=get_archive_number)
        for table in reversed(tables[1:]):
            if get_prev_table(table) in tables:
                logger.info("Moving %s to %s", table, get_next_table(table))
                yt.move(table, get_next_table(table))
    
def archive_event_log(archive_size_limit):
    logger.info("Archive prefix of event log table")
    row_count_to_archive = get_desired_row_count_to_archive(archive_size_limit)
    archived_row_count = get_archived_row_count()
    assert archived_row_count == 0

    with yt.Transaction():
        data_size_per_job = min(10 * 1024 ** 3, int((1024 ** 3) / get_archive_compression_ratio()))
        yt.create("table", "event_log.1", attributes={"erasure_code": "lrc_12_2_2", "compression_codec": "zlib6"}, ignore_existing=True)
        yt.run_merge(yt.TablePath("event_log", start_index=0, end_index=row_count_to_archive), yt.TablePath("event_log.1", append=True),
                     spec={"force_transform": True, "data_size_per_job": data_size_per_job})
        yt.set("event_log/@archived_row_count", row_count_to_archive)

def main():
    parser = ArgumentParser(description="Script to rotate scheduler event logs")
    parser.add_argument("--archive-size-limit", type=int, default=500 * 1024 ** 3)
    parser.add_option("--min-portion-to-archive", type=int, default=10 * 1024 ** 3)
    args = parser.parse_args()

    if not fennel_exists():
        logger.error("Event log is not processed by fennel, it is impossible to safely rotate it")
        return 1

    if get_possible_size_to_archive() < args.min_portion_to_archive:
        logger.info("There is now enough data to archive")
        return 0

    erase_archived_prefix()
    rotate_archives(args.archive_size_limit, args.min_portion_to_archive)
    archive_event_log(args.archive_size_limit)
    erase_archived_prefix()


if __name__ == "__main__":
    main()
