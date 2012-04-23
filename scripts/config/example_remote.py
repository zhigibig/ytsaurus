#!/usr/bin/python
from cfglib.ytremote import *
import cfglib.opts as opts

Logging = {
    'writers' : {
        'raw' :
        {
            'type' : 'raw',
            'file_name' : "/yt/%(log_disk)s/data/logs/%(debug_log_path)s"
        },
        'file' :
        {
            'type' : "file",
            'file_name' : "/yt/%(log_disk)s/data/logs/%(log_path)s",
            'pattern' : "$(datetime) $(level) $(category) $(message)"
        }
    },
    'rules' : [
        { 
            'categories' : [ "*" ],
            'min_level' : "debug",
            'writers' : [ "raw" ]
        },
        {
            'categories' : [ "*" ],
            'min_level' : "info",
            'writers' : [ "file" ]
        }
    ]
}


MasterAddresses = opts.limit_iter('--masters', ['meta01-00%dg:9000' % i for i in xrange(1, 4)])

class Base(AggrBase):
    path = opts.get_string('--name', 'control')
    base_dir = '/yt/disk1/data'
    libs = [
        '/home/yt/build/lib/libstlport.so.5.2',
        '/home/yt/build/lib/libyajl.so.2',
        '/home/yt/build/lib/libytext-fastlz.so.0.1',
        '/home/yt/build/lib/libytext-json.so',
        '/home/yt/build/lib/libytext-lz4.so.0.1',
        '/home/yt/build/lib/libytext-minilzo.so.2.0',
        '/home/yt/build/lib/libytext-quality-misc.so',
        '/home/yt/build/lib/libytext-quality-netliba_v6.so',
        '/home/yt/build/lib/libytext-snappy.so.1.0',
        '/home/yt/build/lib/libytext-zlib.so.1.2.3'
    ]
    
    def get_log(cls, fd):
        print >>fd, shebang
        print >>fd, 'rsync %s:%s %s' % (cls.host, cls.config['logging']['writers']['file']['file_name'], cls.local_dir)
    
class Server(Base, RemoteServer):
    bin_path = '/home/yt/build/bin/ytserver'

    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -f %s' % cls.log_path
        print >>fd, 'rm -f %s' % cls.debug_log_path
    
class Master(Server):
    base_dir = '/yt/disk2/data'
    address = Subclass(MasterAddresses)
    params = Template('--master --config %(config_path)s')

    log_disk = 'disk2'
    log_path = Template("master-%(__name__)s.log")
    debug_log_path = Template("master-%(__name__)s.debug.log")

    config = Template({
        'meta_state' : {
            'leader_committer' : {
                'max_batch_delay': 50
            },
            'cell' : {
                'addresses' : MasterAddresses,
                'rpc_port' : 9000
            },
            'snapshots' : {
                'path' : '/yt/disk2/data/snapshots',
            },
            'changelogs' : {
                'path' : '/yt/disk1/data/changelogs',
            },
            'max_changes_between_snapshots' : 1000000,
            'changelog_downloader' : {
                'records_per_request' : 10240
            },
        },
        'chunks' : {
            'registered_holder_timeout' : 180000,
            'jobs' : {
                'min_online_holder_count' : 250,
                'max_lost_chunk_fraction' : 0.01
            }
        },
        'monitoring_port' : 10000, 
        'logging' : Logging
    })
    
    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -f %s' % cls.log_path
        print >>fd, 'rm -f %s' % cls.debug_log_path
        print >>fd, 'rm %s/*' % cls.config['meta_state']['snapshots']['path']
        print >>fd, 'rm %s/*' % cls.config['meta_state']['changelogs']['path']
        
class Scheduler(Server):
    base_dir = '/yt/disk2/data'
    address = MasterAddresses[1]
    params = Template('--scheduler --config %(config_path)s')

    log_disk = 'disk2'
    log_path = "scheduler.log"
    debug_log_path = "scheduler.debug.log"

    config = Template({
        'masters' : {
            'addresses' : MasterAddresses
        },
        'scheduler' : {   
            'strategy' : 'fifo'
        },
        'rpc_port' : 9001,
        'monitoring_port' : 10001, 
        'logging' : Logging
    })

CleanCache = FileDescr('clean_cache', ('aggregate', 'exec'))
DoCleanCache = FileDescr('do_clean_cache', ('remote', 'exec'))

class Holder(Server):
    files = Server.files + [CleanCache, DoCleanCache]

    # XXX(sandello): I have taken one group for my personal experiments
    # for deploying YT via Chef. Hence nodes 670-699 are unused in this deployment.
    groupid = Subclass(xrange(9))
    nodeid = Subclass(xrange(30), 1)

    log_disk = 'disk2'
    log_path = Template("holder-%(groupid)d-%(nodeid)d.log")
    debug_log_path = Template("holder-%(groupid)d-%(nodeid)d.debug.log")
    
    @propmethod
    def host(cls):
        return 'n01-0%dg' % (400 + 30 * cls.groupid + cls.nodeid)
    
    params = Template('--node --config %(config_path)s')

    proxyLogging = deepcopy(Logging)
    proxyLogging['writers']['raw']['file_name'] = 'job_proxy.log'
    proxyLogging['writers']['file']['file_name'] = 'job_proxy.debug.log'

    storeQuota = 1700 * 1024 * 1024 * 1024 # the actual limit is ~1740
    cacheQuota = 1 * 1024 * 1024 * 1024
    config = Template({ 
        'masters' : {
            'addresses' : MasterAddresses
        },
        'chunk_holder' : {
            'store_locations' : [
                { 'path' : '/yt/disk1/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk2/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk3/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk4/data/chunk_store', 'quota' : storeQuota }
            ],
            'cache_location' : {
                'path' : '/yt/disk1/data/chunk_cache', 'quota' : cacheQuota
            },
            'cache_remote_reader' : { 
                'publish_peer' : 'true'
            },
            'full_heartbeat_timeout' : 180000,
            'max_cached_blocks_size' : 10 * 1024 * 1024 * 1024,
            'max_cached_readers' : 256,
            'response_throttling_size' : 500 * 1024 * 1024
        },
        'exec_agent' : {
            'environment_manager' : {
                'environments' : {
                    'default' : {
                        'type' : 'unsafe'
                    }
                }
            },
            'job_manager': {
                'slot_location' : '%(work_dir)s/slots',
            },
            'job_proxy_logging' : proxyLogging,
        },
        'rpc_port' : 9002,
        'monitoring_port' : 10002, 
        'logging' : Logging
    })
    
    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -f %s' % cls.log_path
        print >>fd, 'rm -f %s' % cls.debug_log_path
        for location in cls.config['chunk_holder']['store_locations']:
            print >>fd, 'rm -rf %s' % location['path']
        print >>fd, 'rm -rf %s' % cls.config['chunk_holder']['cache_location']['path']

    def do_clean_cache(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -rf %s' % cls.config['chunk_holder']['cache_location']['path']


class Driver(Base, RemoteNode):
    bin_path = '/home/yt/build/bin/yt'

    nodeid = Subclass(xrange(400, 700))
    host = Template('n01-0%(nodeid)dg')

    params = Template('--config %(config_path)s')

    log_disk = 'disk2'
    log_path = Template("driver-%(nodeid)d.log")
    debug_log_path = Template("driver-%(nodeid)d.debug.log")

    stderr_path = Template('%(work_dir)s/driver.err')

    config = Template({ 
        'masters' : { 'addresses' : MasterAddresses },
        'logging' : Logging
    })

    def run(cls, fd):
        print >>fd, shebang
        print >>fd, cmd_ssh % (cls.host, 
            'start-stop-daemon -d ./ -b --exec %s --pidfile %s/pid -m -S' % (cls.do_run_path, cls.work_dir))

    def do_run(cls, fd):
        print >>fd, shebang
        print >>fd, ulimit
        print >>fd, cls.export_ld_path
        suffix = '%s 2>&1 1>%s' % (cls.params, cls.stderr_path)
        prefix = '%s/%s' % (cls.work_dir, cls.binary)
        path = '//tmp/table%d' % cls.nodeid
        print >>fd, ' '.join([prefix, 'create table', path, suffix])
        print >>fd, 'for i in {1..3000}'
        print >>fd, 'do'
        print >>fd, ' '.join([prefix, 'write', path, '{hello=world}', suffix])
        print >>fd, 'done'

#    def do_stop(cls, fd):
#        print >>fd, shebang
#        print >>fd, 'killall yt'
    
    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -f %s' % cls.log_path
        print >>fd, 'rm -f %s' % cls.debug_log_path
        print >>fd, 'rm -f %s' % cls.stderr_path

configure(Base)
    
