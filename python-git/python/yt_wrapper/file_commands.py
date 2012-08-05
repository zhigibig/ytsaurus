import config
from common import require, YtError, add_quotes
from path_tools import escape_path
from http import make_request
from tree_commands import remove, exists, set_attribute, set, mkdir, find_free_subpath

import os

def download_file(path):
    content = make_request("GET", "download", {"path": escape_path(path)}, raw_response=True)
    return content.iter_lines()

def upload_file(filename, yt_filename=None, destination=None, placement_strategy=None):
    """
    Upload file to specified path.
    If destination is not specified, than name is determined by placement strategy.
    If placement_strategy equals "replace" or "ignore", then destination is set up
    'config.FILE_STORAGE/basename'. In "random" case (default) destination is set up
    'config.FILE_STORAGE/basename<random_suffix>'
    If yt_filename is specified than set it as ytable system name
    (name that would be visible in operations).
    """
    require(os.path.isfile(filename),
            YtError("Upload: %s should be file" % filename))

    if placement_strategy is None: 
        placement_strategy = "random"
    require(placement_strategy in ["replace", "ignore", "random"],
            YtError("Incorrect file placement strategy " + placement_strategy))
    
    basename = os.path.basename(filename)
    if yt_filename is None:
        yt_filename = basename
    if destination is None:
        mkdir(config.FILE_STORAGE)
        destination = os.path.join(config.FILE_STORAGE, basename)
        if placement_strategy == "random":
            destination = find_free_subpath(destination)
        if placement_strategy == "replace" and exists(destination):
            yt.remove(destination)
        if placement_strategy == "ignore" and exists(destination):
            return
    
    operation = make_request(
            "PUT", "upload", 
            {
                "path": escape_path(destination),
                "file_name": yt_filename
            },
            data=open(filename))
    # Set executable flag if need
    if os.access(filename, os.X_OK):
        set_attribute(destination, "executable", add_quotes("true"))
    return destination

