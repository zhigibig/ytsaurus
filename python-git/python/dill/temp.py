#!/usr/bin/env python

"""
Methods for serialized objects (or source code) stored in temporary files.
"""

def dump_source(object, **kwds):
    """ write object source to a NamedTemporaryFile (instead of pickle.dump)

NOTE: Keep the return value for as long as you want your file to exist !
    """
    from source import getsource
    import tempfile
    alias = kwds.pop('alias', '') #XXX: include an alias at the end of the file
    #XXX: assumes '.' is writable and on $PYTHONPATH
    file = tempfile.NamedTemporaryFile(suffix='.py', **kwds)
    file.write(''.join(getsource(object, alias=alias)))
    file.flush()
    return file

def dump(object, **kwds):
    """ pickle.dump of object to a NamedTemporaryFile """
    import dill as pickle
    import tempfile
    file = tempfile.NamedTemporaryFile(**kwds)
    pickle.dump(object, file)
    file.flush()
    return file

