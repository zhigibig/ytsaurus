import fennel

from tornado import iostream
from tornado import ioloop

import pytest
import mock
import datetime
import unittest
import subprocess


def test_compression_external():
    p = subprocess.Popen(["gzip", "-d"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdoutdata, stderrdata = p.communicate(fennel.gzip_compress("Hello"))
    assert stdoutdata == "Hello"
    assert not stderrdata


def test_compression_internal():
    assert fennel.gzip_decompress(fennel.gzip_compress("Hello")) == "Hello"


def test_event_log_timestamp_parse():
    d = datetime.datetime.strptime("2014-10-02T15:31:24.887386Z".split(".")[0], "%Y-%m-%dT%H:%M:%S")
    assert d.hour == 15
    assert d.minute == 31
    assert d.second == 24
    assert d.date().year == 2014
    assert d.date().month == 10
    assert d.date().day == 2


def test_normilize_timestamp():
    assert fennel.normilize_timestamp("2014-10-02T15:31:24.887386Z") == "2014-10-02 15:31:24"
    assert fennel.normilize_timestamp("2014-10-10T14:07:22.295882Z") == "2014-10-10 14:07:22"


def test_tskv_value_escape_encode():
    assert fennel.escape_encode("Hello") == "Hello"
    assert fennel.escape_encode("Hello\nworld") == "Hello\\nworld"
    assert fennel.escape_encode("\\ is slash, but \0 is zero char") == "\\\\ is slash, but \\0 is zero char"


def test_tskv_key_escape_encode():
    assert fennel.escape_encode("Complex=key", escape_chars=fennel.TSKV_KEY_ESCAPE) == "Complex\\=key"


def check_identity(identity, parameter):
    assert identity(parameter) == parameter


def test_tskv_escape_value_encode_decode():
    def identity(line):
        return fennel.escape_decode(fennel.escape_encode(line))

    check_identity(identity, "\\ is slash, but \0 is zero char")
    check_identity(identity, "Hello")
    check_identity(identity, "Hello\nworld")


def compare_types(left, right):
    if isinstance(left, basestring) and isinstance(right, basestring):
        pass
    else:
        assert type(left) == type(right)


def compare(left, right):
    compare_types(left, right)
    if isinstance(left, dict):
        assert len(left.keys()) == len(right.keys())
        for k in left.keys():
            compare(left[k], right[k])
    else:
        assert left == right


def test_convert_to_from():
    dataset = [
        dict(key1="value1", key2="value2"),
        dict(ks="string", ki=3),
        dict(key=dict(s1="v1", s2="v2"), other_key="data")
#        dict(key="{}")
    ]
    for data in dataset:
        data_after = fennel.convert_from(fennel.convert_to(data))
        compare(data, data_after)


@pytest.fixture
def fake_state():
    state = fennel.State(event_log=mock.Mock(name="event_log"))
    state._log_broker = mock.Mock(name="log_broker")
    return state


def test_on_skip_new(fake_state):
    fake_state.on_skip(10)
    assert fake_state._last_seqno == 10


def test_on_skip_old(fake_state):
    fake_state._last_seqno = 20
    fake_state.on_skip(10)
    assert fake_state._last_seqno == 20


def test_on_save_ack_next(fake_state):
    assert fake_state._last_seqno == 0
    fake_state.on_save_ack(1)
    assert fake_state._last_seqno == 1


def test_on_save_ack_not_next(fake_state):
    assert fake_state._last_seqno == 0
    fake_state.on_save_ack(2)
    assert fake_state._last_seqno == 2


def test_on_save_ack_reordered(fake_state):
    assert fake_state._last_seqno == 0
    fake_state.on_save_ack(2)
    fake_state.on_save_ack(1)
    assert fake_state._last_seqno == 2


def test_on_skip_removes_old_acks(fake_state):
    fake_state.on_save_ack(2)
    fake_state.on_save_ack(11)
    fake_state.on_skip(10)
    assert fake_state._last_seqno == 11


def test_do_not_save(fake_state):
    fake_state._last_saved_seqno = 1
    fake_state.maybe_save_another_chunk()
    assert fake_state._last_saved_seqno == 1


def test_save(fake_state):
    fake_state._last_saved_seqno = 0
    fake_state.maybe_save_another_chunk()
    assert fake_state._last_saved_seqno == 1


@pytest.fixture
def fake_session():
    return fennel.Session(
        mock.Mock(spec=fennel.State, name="state"),
        mock.Mock(spec=fennel.LogBroker, name="logbroker"),
        mock.Mock(name="ioloop"),
        mock.Mock(name="IOStreamClass"))


def test_session_process_data_skip(fake_session):
    fake_session.process_data("skip    chunk=16        offset=256      seqno=256")
    assert not fake_session._state.on_save_ack.called
    fake_session._state.on_skip.assert_called_with(256)


def test_session_process_data_ack(fake_session):
    fake_session.process_data("chunk=19        offset=304      seqno=304       part_offset=111")
    assert not fake_session._state.on_skip.called
    fake_session._state.on_save_ack.assert_called_with(304)


def test_session_process_data_no_seqno(fake_session):
    fake_session.process_data("offset=304      part_offset=111")
    assert not fake_session._state.on_save_ack.called
    assert not fake_session._state.on_save_ack.called


def test_session_process_data_seqno_is_nan(fake_session):
    fake_session.process_data("offset=304      seqno=xxx      part_offset=111")
    assert not fake_session._state.on_save_ack.called
    assert not fake_session._state.on_save_ack.called

def test_session_process_data_bad_record(fake_session):
    fake_session.process_data("offset=304      seqno      part_offset=111")
    assert not fake_session._state.on_save_ack.called
    assert not fake_session._state.on_save_ack.called


def test_session_read_good_id(fake_session):
    data = """HTTP/1.1 200 OK
Server: nginx/1.4.4
Date: Wed, 19 Mar 2014 11:09:54 GMT
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: keep-alive
Vary: Accept-Encoding
Session: 00291e7c-eedf-42cd-99cc-f18331b9db77
Seqno: 3488
Lines: 218
PartOffset: 396
Topic: rt3.fol--other
Partition: 0"""
    fake_session.read_metadata(data)
    assert fake_session._id == "00291e7c-eedf-42cd-99cc-f18331b9db77"


class StreamToKafka(object):
    def __init__(self, session_data, store_data, io_loop, stream_holder=None, counters=None):
        self._session_data = session_data
        self._store_data = store_data
        self._data = None
        self._index = 0
        self._io_loop = io_loop
        self._output_data = []
        self._close_callback = None
        self._stream_holder = stream_holder
        self._counters = counters or 0

    def write(self, data):
        if len(self._output_data) == 0:
            name = None
            if data.startswith("GET /rt/session"):
                name = "session"
                self._data = self._session_data
            else:
                name = "store"
                self._data = self._store_data

            if name is not None and self._stream_holder is not None:
                self._stream_holder[name] = self

        self._output_data.append(data)

    def connect(self, endpoint, callback):
        if self._counters._fail_connections > 0:
            self._counters._fail_connections -= 1
            if self._close_callback:
                self._io_loop.add_callback(self._close_callback)
        else:
            self._io_loop.add_callback(callback)

    def read_until(self, delimiter, callback):
        index = self._data.find(delimiter, self._index)
        if index != -1:
            index += len(delimiter)
            self._io_loop.add_callback(callback, self._data[self._index:index])
            self._index = index
        else:
            self._io_loop.stop()

    def read_bytes(self, size, callback):
        if self._index + size <= len(self._data):
            self._io_loop.add_callback(callback, self._data[self._index:self._index + size])
            self._index += size
        else:
            self._io_loop.stop()

    def read_until_close(self, callback, streaming_callback):
        pass

    def set_close_callback(self, callback):
        self._close_callback = callback

    def close():
        self._io_loop.stop()


session_data = """HTTP/1.1 200 OK
Server: nginx/1.4.4
Date: Wed, 19 Mar 2014 11:09:54 GMT
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: keep-alive
Vary: Accept-Encoding
Session: 00291e7c-eedf-42cd-99cc-f18331b9db77
Seqno: 3488
Lines: 218
PartOffset: 396
Topic: rt3.fol--other
Partition: 0\r\n\r\n5\r\nping
"""


class IOLoopedTestCase(unittest.TestCase):
    def setUp(self):
        self.stream_holder = {}
        self.force_stop = False
        self.io_loop = ioloop.IOLoop()
        self.io_loop.add_timeout(datetime.timedelta(seconds=self.get_second_to_stop()), self.stop)
        self._fail_connections = 0

    def tearDown(self):
        pass

    def get_second_to_stop(self):
        return 1

    def test_empty(self):
        pass

    def start(self):
        self.io_loop.start()

    def stop(self):
        self.force_stop = True
        self.io_loop.stop()

    def stream_factory(self, s, io_loop):
        return StreamToKafka(
            session_data,
            "",
            io_loop,
            stream_holder=self.stream_holder,
            counters=self)


class TestSession(IOLoopedTestCase):
    def test_connect(self):
        s = fennel.Session(
            mock.Mock(name="state"),
            mock.Mock(name="log_broker", get_endpoint=lambda: (u'kafka01ft.stat.yandex.net', 80)),
            self.io_loop,
            self.stream_factory)
        s.connect()
        self.start()
        assert s._id is not None
        assert not self.force_stop


class TestSessionReconnect(IOLoopedTestCase):
    def test_basic(self):
        self._fail_connections = 1
        s = fennel.Session(
            mock.Mock(name="state"),
            mock.Mock(name="log_broker", get_endpoint=lambda: (u'kafka01ft.stat.yandex.net', 80)),
            self.io_loop,
            self.stream_factory)
        s.connect()
        self.start()
        assert s._id is not None
        assert not self.force_stop

    def get_second_to_stop(self):
        return 4


class TestSaveChunk(IOLoopedTestCase):
    def test_basic(self):
        state = mock.Mock(name="state")
        l = fennel.LogBroker(
            state,
            io_loop=self.io_loop,
            IOStreamClass = self.stream_factory)
        l.get_endpoint = lambda: (u'kafka01ft.stat.yandex.net', 80)
        l.start()

        def save_all():
            l.save_chunk(0, [{"key0":"value0", "timestamp": "2014-10-02T15:31:24.887386Z"}])
            l.save_chunk(1, [{"key1":"value1",  "timestamp": "2014-10-02T15:31:24.887386Z"}])
            l.save_chunk(2, [{"key2":"value2",  "timestamp": "2014-10-02T15:31:24.887386Z"}, {"key3":"value3",  "timestamp": "2014-10-02T15:31:24.887386Z"}])
        state.on_session_changed = save_all

        self.start()
        # the first one stops when the fake session stream is over
        self.start()

        assert "session" in self.stream_holder
        assert "store" in self.stream_holder

        chunks = [fennel.parse_chunk(x) for x in self.stream_holder["store"]._output_data[1:]]
        assert len(chunks) == 3
        assert chunks[0][0]["key0"] == "value0"
        assert chunks[1][0]["key1"] == "value1"
        assert chunks[2][1]["key3"] == "value3"


def xtest_session_integration():
    import logging
    logging.basicConfig(level=logging.DEBUG)

    io_loop = ioloop.IOLoop()
    def stop():
        io_loop.stop()

    io_loop.add_timeout(datetime.timedelta(seconds=3), stop)
    s = fennel.Session(mock.Mock(), mock.Mock(), io_loop, iostream.IOStream, source_id="WHt4FAA")
    s.connect()
    io_loop.start()
    assert s._id is not None
