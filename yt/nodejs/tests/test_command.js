var qs = require("querystring");

var YtDriver = require("../lib/driver").that;
var YtCommand = require("../lib/command").that;

var utils = require("../lib/utils");

////////////////////////////////////////////////////////////////////////////////

var ask = require("./common_http").ask;
var srv = require("./common_http").srv;

// This will spawn a (mock of a) real API server.
function spawnServer(driver, watcher, done) {
    var logger = stubLogger();
    return srv(function(req, rsp) {
        var pause = utils.Pause(req);
        req.authenticated_user = "root";
        return (new YtCommand(
            logger, driver, watcher,
            HTTP_HOST + ":" + HTTP_PORT,
            false, pause
        )).dispatch(req, rsp);
    }, done);
}

// This stub provides a real driver instance which simply pipes all data through.
function stubDriver(echo) {
    var config = {
        "low_watermark" : 100,
        "high_watermark" : 200,
        "proxy" : {
            "driver" : { "masters" : { "addresses" : [ "localhost:0" ] } },
            "logging" : { "rules" : [], "writers" : {} }
        }
    };

    return new YtDriver(config, !!echo);
}

// This stub provides a constant watcher which is either choking or not.
function stubWatcher(is_choking) {
    return { tackle: function(){}, is_choking: function() { return is_choking; } };
}

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - http method selection", function() {
    before(function(done) {
        this.server = spawnServer(stubDriver(true), stubWatcher(false), done);
    });

    after(function(done) {
        this.server.close(done);
        this.server = null;
    });

    [ "/get", "/download", "/read" ]
    .forEach(function(entry_point) {
        it("should use GET for " + entry_point, function(done) {
            ask("GET", entry_point, {},
            function(rsp) { rsp.should.be.http2xx; }, done).end();
        });
    });

    [ "/set", "/upload", "/write" ]
    .forEach(function(entry_point) {
        it("should use PUT for " + entry_point, function(done) {
            ask("PUT", entry_point, {},
            function(rsp) { rsp.should.be.http2xx; }, done).end();
        });
    });

    [ "/map", "/reduce", "/sort" ]
    .forEach(function(entry_point) {
        it("should use POST for " + entry_point, function(done) {
            ask("POST", entry_point, {},
            function(rsp) { rsp.should.be.http2xx; }, done).end("{}");
        });
    });
});

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - command name", function() {
    before(function(done) {
        this.server = spawnServer(stubDriver(true), stubWatcher(false), done);
    });

    after(function(done) {
        this.server.close(done);
        this.server = null;
    });

    it("should allow good names", function(done) {
        ask("GET", "/get", {},
        function(rsp) { rsp.should.be.http2xx; }, done).end();
    });

    it("should disallow bad names", function(done) {
        ask("GET", "/$$$", {},
        function(rsp) {
            rsp.statusCode.should.eql(400);
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should return 404 when the name is unknown", function(done) {
        ask("GET", "/unknown_but_valid_name", {},
        function(rsp) {
            rsp.statusCode.should.eql(404);
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should display a reference when the name is empty", function(done) {
        ask("GET", "", {},
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.should.have.content_type("application/json");

            var body = JSON.parse(rsp.body);
            body.should.be.instanceof(Array);
            body.forEach(function(item) {
                item.should.have.property("name");
                item.should.have.property("input_type");
                item.should.have.property("output_type");
                item.should.have.property("is_volatile");
                item.should.have.property("is_heavy");
            });
        }, done).end();
    });
});

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - command heaviness", function() {
    var driver = stubDriver(true);

    [ "download", "upload", "read", "write" ]
    .forEach(function(name) {
        it("should affect '" + name + "'", function() {
            driver.find_command_descriptor(name).is_heavy.should.be.true;
        });
    });

    describe("when there is no workload", function() {
        before(function(done) {
            this.server = spawnServer(driver, stubWatcher(false), done);
        });

        after(function(done) {
            this.server.close(done);
            this.server = null;
        });

        it("should allow light commands ", function(done) {
            ask("GET", "/get", {},
            function(rsp) { rsp.should.be.http2xx; }, done).end();
        });

        it("should allow heavy commands ", function(done) {
            ask("GET", "/read", {},
            function(rsp) { rsp.should.be.http2xx; }, done).end();
        });
    });

    describe("when there is workload", function() {
        before(function(done) {
            this.server = spawnServer(driver, stubWatcher(true), done);
        });

        after(function(done) {
            this.server.close(done);
            this.server = null;
        });

        it("should allow light commands ", function(done) {
            ask("GET", "/get", {},
            function(rsp) { rsp.should.be.http2xx; }, done).end();
        });

        it("should disallow heavy commands ", function(done) {
            ask("GET", "/read", {},
            function(rsp) { rsp.statusCode.should.eql(503); }, done).end();
        });
    });
});

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - command parameters", function() {
    beforeEach(function(done) {
        this.driver = stubDriver(true);
        this.server = spawnServer(this.driver, stubWatcher(false), done);
        this.stub   = sinon.spy(this.driver, "execute");
    });

    afterEach(function(done) {
        this.server.close(done);
        this.driver = null;
        this.server = null;
        this.stub   = null;
    });

    it("should set no defaults", function(done) {
        var stub = this.stub;
        ask("GET", "/get",
        {},
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Get().should.be.empty;
        }, done).end();
    });

    it("should take query string parameters", function(done) {
        var stub = this.stub;
        var params = { "who" : "me", "path" : "/", "foo" : "bar" };
        ask("GET", "/get?" + qs.encode(params),
        {},
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Get().should.eql(params);
        }, done).end();
    });

    it("should take header parameters", function(done) {
        var stub = this.stub;
        var params = { "who" : "me", "path" : "/", "foo" : "bar" };
        ask("GET", "/get",
        { "X-YT-Parameters" : JSON.stringify(params) },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Get().should.eql(params);
        }, done).end();
    });

    it("should take body parameters for POST methods", function(done) {
        var stub = this.stub;
        var params = { "who" : "me", "path" : "/", "foo" : "bar" };
        ask("POST", "/map",
        { "Content-Type" : "application/json" },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Get().should.eql(params);
        }, done).end(JSON.stringify(params));
    });

    it("should set proper precedence", function(done) {
        //  URL: a1 a2 a3
        // HEAD:    a2 a3 a4
        // BODY:       a3 a4 a5
        var stub = this.stub;
        var from_url  = qs.encode({ a1: "foo", a2: "bar", a3: "baz" });
        var from_head = JSON.stringify({ a2: "xyz", a3: "www", a4: "abc" });
        var from_body = JSON.stringify({ a3: "pooh", a4: "puff", a5: "blah" });
        ask("POST", "/map?" + from_url,
        { "Content-Type" : "application/json", "X-YT-Parameters" : from_head },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Get().should.eql({
               a1: "foo", a2: "xyz", a3: "pooh", a4: "puff", a5: "blah"
            });
        }, done).end(from_body);
    });

    it("should properly treat attributes in JSON", function(done) {
        var stub = this.stub;
        ask("POST", "/map",
        { "Content-Type" : "application/json" },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Print().should.eql('{"path"=<"append"="true">"//home"}');
        }, done).end('{"path":{"$value":"//home","$attributes":{"append":"true"}}}');
    });

    it("should properly treat binary strings in JSON", function(done) {
        var stub = this.stub;
        ask("POST", "/map",
        { "Content-Type" : "application/json" },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.body.should.be.empty;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[8].Print().should.eql('{"hello"="world"}');
        }, done).end('{"&aGVsbG8=":"&d29ybGQ="}');
    });
});

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - input format selection", function() {
    beforeEach(function(done) {
        this.driver = stubDriver(true);
        this.server = spawnServer(this.driver, stubWatcher(false), done);
        this.stub   = sinon.spy(this.driver, "execute");
    });

    afterEach(function(done) {
        this.server.close(done);
        this.driver = null;
        this.server = null;
        this.stub   = null;
    });

    it("should use 'json' as a default for structured data", function(done) {
        var stub = this.stub;
        ask("PUT", "/set", {},
        function(rsp) {
            rsp.should.be.http2xx;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[4].Print().should.eql('"json"');
        }, done).end();
    });

    it("should use 'yson' as a default for tabular data", function(done) {
        var stub = this.stub;
        ask("PUT", "/write", {},
        function(rsp) {
            rsp.should.be.http2xx;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[4].Print().should.eql('<"format"="text">"yson"');
        }, done).end();
    });

    it("should use 'yson' as a default for binary data", function(done) {
        var stub = this.stub;
        ask("PUT", "/upload", {},
        function(rsp) {
            rsp.should.be.http2xx;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[4].Print().should.eql('"yson"');
        }, done).end();
    });

    it("should respect Content-Type header", function(done) {
        var stub = this.stub;
        ask("PUT", "/write",
        { "Content-Type" : "text/tab-separated-values" },
        function(rsp) {
            rsp.should.be.http2xx;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[4].Print().should.eql('"dsv"');
        }, done).end();
    });

    it("should respect custom header with highest precedence and discard mime-type accordingly", function(done) {
        var stub = this.stub;
        ask("PUT", "/write",
        {
            "Content-Type" : "text/tab-separated-values",
            "X-YT-Input-Format" : JSON.stringify({
                $attributes: { "foo" : "bar" },
                $value: "yson"
            })
        },
        function(rsp) {
            rsp.should.be.http2xx;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[4].Print().should.eql('<"foo"="bar">"yson"');
        }, done).end();
    });

    it("should fail with bad Content-Type header", function(done) {
        var stub = this.stub;
        ask("PUT", "/write",
        { "Content-Type" : "i-am-a-cool-hacker", },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should fail with bad X-YT-Input-Format header", function(done) {
        var stub = this.stub;
        ask("PUT", "/write",
        { "X-YT-Input-Format" : "i-am-a-cool-hacker666{}[]", },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should fail with non-existent format", function(done) {
        var stub = this.stub;
        ask("PUT", "/write",
        { "X-YT-Input-Format" : '"uberzoldaten"' },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });
});

////////////////////////////////////////////////////////////////////////////////

describe("YtCommand - output format selection", function() {
    beforeEach(function(done) {
        this.driver = stubDriver(true);
        this.server = spawnServer(this.driver, stubWatcher(false), done);
        this.stub   = sinon.spy(this.driver, "execute");
    });

    afterEach(function(done) {
        this.server.close(done);
        this.driver = null;
        this.server = null;
        this.stub   = null;
    });

    it("should use application/json as a default for structured data", function(done) {
        var stub = this.stub;
        ask("GET", "/get", {},
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.should.have.content_type("application/json");
            stub.should.have.been.calledOnce;
            stub.firstCall.args[7].Print().should.eql('"json"');
        }, done).end();
    });

    it("should use application/x-yt-yson-text as a default for tabular data", function(done) {
        var stub = this.stub;
        ask("GET", "/read", {},
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.should.have.content_type("application/x-yt-yson-text");
            stub.should.have.been.calledOnce;
            stub.firstCall.args[7].Print().should.eql('<"format"="text">"yson"');
        }, done).end();
    });

    it("should use application/octet-stream as a default for binary data", function(done) {
        var stub = this.stub;
        ask("GET", "/download", {},
        function(rsp) {
            rsp.should.be.http2xx;
            // XXX(sandello): Fix me.
            rsp.should.have.content_type("text/plain");
            stub.should.have.been.calledOnce;
            stub.firstCall.args[7].Print().should.eql('"yson"');
        }, done).end();
    });

    it("should respect Accept header", function(done) {
        var stub = this.stub;
        ask("GET", "/read",
        { "Accept" : "text/tab-separated-values" },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.should.have.content_type("text/tab-separated-values");
            stub.should.have.been.calledOnce;
            stub.firstCall.args[7].Print().should.eql('"dsv"');
        }, done).end();
    });

    it("should respect custom header with highest precedence and discard mime-type accordingly", function(done) {
        var stub = this.stub;
        ask("GET", "/read",
        {
            "Accept" : "text/tab-separated-values",
            "X-YT-Output-Format" : JSON.stringify({
                $attributes: { "foo" : "bar" },
                $value: "yson"
            })
        },
        function(rsp) {
            rsp.should.be.http2xx;
            rsp.should.not.have.content_type;
            stub.should.have.been.calledOnce;
            stub.firstCall.args[7].Print().should.eql('<"foo"="bar">"yson"');
        }, done).end();
    });

    it("should fail with bad Accept header", function(done) {
        var stub = this.stub;
        ask("GET", "/read",
        { "Accept" : "i-am-a-cool-hacker", },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should fail with bad X-YT-Output-Format header", function(done) {
        var stub = this.stub;
        ask("GET", "/read",
        { "X-YT-Output-Format" : "i-am-a-cool-hacker666{}[]", },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });

    it("should fail with non-existing format", function(done) {
        var stub = this.stub;
        ask("GET", "/read",
        { "X-YT-Output-Format" : '"uberzoldaten"' },
        function(rsp) {
            rsp.should.be.yt_error;
        }, done).end();
    });
});
