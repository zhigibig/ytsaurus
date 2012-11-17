import config
from common import YtError, require, parse_bool, flatten, get_value
from path_tools import dirs, split_table_ranges
from format import JsonFormat
from transaction_commands import _make_transactioned_request

from yt.yson.yson_types import YSONString

import os
import string
import random
from copy import deepcopy
import simplejson as json

def get(path, attributes=None, format=None, spec=None):
    """
    Gets the tree growning from path.
    attributes -- attributes to provide for each node in the response.
    format -- output format (by default it is json that automatically parsed to python structure).

    Be carefull: attributes have weird representation in json format.
    """
    return _make_transactioned_request(
        "get",
        {
            "path": path,
            "attributes": get_value(attributes, []),
            "spec": {} if spec is None else spec
        },
        format=format)

def set(path, value):
    """
    Sets the value by path. Value should json-able object.
    """
    return _make_transactioned_request(
        "set",
        {"path": path},
        data=json.dumps(value),
        format=JsonFormat())

def copy(source_path, destination_path):
    return _make_transactioned_request(
        "copy",
        {
            "source_path": source_path,
            "destination_path": destination_path
        })

def move(source_path, destination_path):
    return _make_transactioned_request(
        "move",
        {
            "source_path": source_path,
            "destination_path": destination_path
        })

def list(path, max_size=1000):
    """
    Lists all items in the path. Paht should be map_node or list_node.
    In case of map_node it returns keys of the node.
    """
    return _make_transactioned_request(
        "list",
        {
            "path": path,
            "max_size": max_size
        })

def exists(path):
    """
    Checks existance of the path. Strip table ranges from the end of the path.
    """
    return parse_bool(
        _make_transactioned_request(
            "exists",
             {"path": split_table_ranges(path)[0]}))

def remove(path, recursive=False, check_existance=False):
    """
    Removes given path. By default it should exists and represent table of file.
    """
    if check_existance and not exists(path):
        return
    if not recursive:
        # TODO: remake for command argument
        require(get_type(path) in ["table", "file"],
                YtError("Can not delete directory, set recursive=True"))

    _make_transactioned_request("remove", {"path": path})

def remove_with_empty_dirs(path):
    """
    Removes path and all empty dirs that appear after deletion.
    """
    while True:
        remove(path, recursive=True)
        path = os.path.dirname(path)
        if path == "//" or list(path):
            break

def mkdir(path, recursive=None):
    """
    Creates directiry. By default parent directory should exist.
    """
    if recursive is None:
        recursive = config.MKDIR_RECURSIVE
    if recursive:
        create = False
        for dir in dirs(path):
            if not create and not exists(dir):
                create = True
            if create:
                mkdir(dir, False)
    else:
        _make_transactioned_request("create", {"path": path, "type": "map_node"})

# TODO: maybe remove this methods
def get_attribute(path, attribute, default=None):
    if default is not None and attribute not in list_attributes(path):
        return default
    return get("%s/@%s" % (path, attribute))

def has_attribute(path, attribute):
    return exists("%s/@%s" % (path, attribute))

def set_attribute(path, attribute, value):
    return set("%s/@%s" % (path, attribute), value)

def list_attributes(path, attribute_path=""):
    return list("%s/@%s" % (path, attribute_path))

def get_type(path):
    return get_attribute(path, "type")


def find_free_subpath(path):
    """
    Searches free node started with path.
    Path can have form {dir}/{prefix}.
    """
    if not path.endswith("/") and not exists(path):
        return path
    LENGTH = 10
    char_set = string.ascii_lowercase + string.ascii_uppercase + string.digits
    while True:
        name = "%s%s" % (path, "".join(random.sample(char_set, LENGTH)))
        if not exists(name):
            return name

def search(root="/", node_type=None, path_filter=None, object_filter=None, attributes=None):
    """
    Searches all objects in root that have specified node_type,
    satisfy path and object filters. Returns list of the objects.
    Adds given attributes to objects.

    It doesn't processed opaque nodes.
    """
    result = []
    def walk(path, object):
        object_type = object["$attributes"]["type"]
        if (node_type is None or object_type == node_type) and \
           (object_filter is None or object_filter(object)) and \
           (path_filter is None or path_filter(path)):
            # TODO(ignat): bad solution, because of embedded attributes
            # have wrong represantation
            rich_path = YSONString(path)
            rich_path.attributes = object["$attributes"]
            result.append(rich_path)
        if object_type == "map_node" and object["$value"] is not None:
            for key, value in object["$value"].iteritems():
                walk('%s/%s' % (path, key), value)
    if attributes is None: attributes = []
    copy_attributes = deepcopy(flatten(attributes))
    copy_attributes.append("type")

    walk(root, get(root, attributes=copy_attributes))
    return result

