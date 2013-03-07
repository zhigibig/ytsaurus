// Dependencies.
var fs = require("fs");
var cluster = require("cluster");
var domain = require("domain");

var connect = require("connect");
var node_static = require("node-static");
var winston = require("winston");
var winston_nssocket = require("winston-nssocket");

var yt = require("yt");

// Debugging stuff.
if (process.env.NODE_DEBUG && /YT(ALL|CLUSTER)/.test(process.env.NODE_DEBUG)) {
    __DBG = function(x) { "use strict"; console.error("YT Cluster Worker:", x); };
    __DBG.$ = true;
} else {
    __DBG = function(){};
    __DBG.$ = false;
}

// Load configuration and set up logging.
var config = JSON.parse(process.env.YT_PROXY_CONFIGURATION);
var logger = new winston.Logger({
    transports: [
        new winston_nssocket.Nssocket({
            host : config.log_address,
            port : config.log_port
        })
    ]
});

var version;

try {
    version = JSON.parse(fs.readFileSync(__dirname + "/../package.json"));
} catch (ex) {
    version = { version : "(development)", versionFull: "(development)", dependencies : {} };
}

// Hoist variable declaration.
var static_server;
var static_server_new;
var dynamic_server;

var violentlyDieTriggered = false;
var violentlyDie = function violentDeath() {
    "use strict";
    if (violentlyDieTriggered) { return; }
    violentlyDieTriggered = true;

    logger.info("Dying", { wid : cluster.worker.id, pid : process.pid });
    process.send({ type: "stopped" });

    process.nextTick(function() {
        cluster.worker.disconnect();
        cluster.worker.destroy();
        process.exit();
    });
};

var gracefullyDieTriggered = false;
var gracefullyDie = function gracefulDeath() {
    "use strict";
    if (gracefullyDieTriggered) { return; }
    gracefullyDieTriggered = true;

    logger.info("Prepairing to die", { wid : cluster.worker.id, pid : process.pid });
    process.send({ type : "stopping" });

    if (dynamic_server && dynamic_server.close) {
        dynamic_server.close(violentlyDie);
    } else {
        violentlyDie();
    }
};

// Fire up the heart.
var supervisor_liveness;

if (!__DBG.$) {
    (function sendHeartbeat() {
        "use strict";
        process.send({ type : "heartbeat" });
        setTimeout(sendHeartbeat, 2000);
    }());

    supervisor_liveness = setTimeout(gracefullyDie, 30000);
}

// Setup message handlers.
process.on("message", function(message) {
    "use strict";
    if (!message || !message.type) {
        return; // Improper message format.
    }

    switch (message.type) {
        case "heartbeat":
            if (supervisor_liveness) {
                clearTimeout(supervisor_liveness);
            }
            supervisor_liveness = setTimeout(gracefullyDie, 15000);
            break;
        case "gracefullyDie":
            gracefullyDie();
            break;
        case "violentlyDie":
            violentlyDie();
            break;
    }
});

// Fire up the head.
logger.info("Starting HTTP proxy worker", { wid : cluster.worker.id, pid : process.pid });

// Setup application server.
// COMPAT(sandello): Remove this.
static_server = new node_static.Server("/usr/share/yt", { cache : 4 * 3600 });
static_server_new = new node_static.Server("/usr/share/yt_new", { cache : 4 * 3600 });

dynamic_server = connect()
    .use(connect.favicon())
    .use(function(req, rsp, next) {
        var domain = domain.create();
        domain.on("error", function(err) {
            var body;
            body = new YtError(err, "Unhandled error in request domain");
            body = body.toJson();
            rsp.writeHead(500, { "Content-Encoding": "application/json" });
            rsp.end(body);
            domain.dispose();
        });
        domain.run(next);
    })
    .use(yt.YtAssignRequestId())
    .use(yt.YtLogRequest(logger))
    .use("/auth", yt.YtAuthenticationApplication(logger, config))
    .use("/ping", function(req, rsp, next) {
        "use strict";
        req.on("end", function() {
            rsp.writeHead(200, { "Content-Length" : 0 });
            rsp.end();
        });
    })
    .use("/hosts", yt.YtHostDiscovery(config.neighbours))
    .use("/_check_availability_time", function(req, rsp, next) {
        "use strict";
        fs.readFile("/var/lock/yt_check_availability_time", function(err, data) {
            if (err) {
                var body = "0";
                rsp.writeHead(200, { "Content-Length" : body.length, "Content-Type" : "text/plain" });
                rsp.end(body);
            } else {
                rsp.writeHead(200, { "Content-Length" : data.length, "Content-Type" : "text/plain" });
                rsp.end(data);
            }
        });
    })
    // Begin of asynchronous middleware.
    .use(function(req, rsp, next) {
        req.pauser = yt.Pause(req);
        req.connection.setNoDelay(true); // Disable Nagle.
        next();
    })
    .use(yt.YtBlackbox(logger, config))
    .use("/api", yt.YtApplication(logger, config))
    .use(function(req, rsp, next) {
        process.nextTick(function() { req.pauser.unpause(); });
        next();
    })
    // End of asynchronous middleware.
    .use("/ui", function(req, rsp, next) {
        "use strict";
        if (req.url === "/") {
            rsp.statusCode = 302;
            rsp.setHeader("Location", "/ui-new/");
            rsp.end();
            return;
        }
        req.on("end", function() {
            static_server.serve(req, rsp);
        });
    })
    .use("/ui-new", function(req, rsp, next) {
        "use strict";
        if (req.url === "/") {
            req.url = "index.html";
        }
        req.on("end", function() {
            static_server_new.serve(req, rsp);
        });
    })
    .use("/__version__", function(req, rsp) {
        "use strict";
        req.on("end", function() {
            rsp.setHeader("Access-Control-Allow-Origin", "*");
            rsp.setHeader("Content-Type", "text/plain");
            rsp.end(version.versionFull);
        });
    })
    .use("/__config__", function(req, rsp) {
        "use strict";
        req.on("end", function() {
            rsp.setHeader("Access-Control-Allow-Origin", "*");
            rsp.setHeader("Content-Type", "application/json");
            rsp.end(JSON.stringify(config));
        });
    })
    .use("/__env__", function(req, rsp) {
        "use strict";
        req.on("end", function() {
            rsp.setHeader("Access-Control-Allow-Origin", "*");
            rsp.setHeader("Content-Type", "application/json");
            rsp.end(JSON.stringify(process.env));
        });
    })
    .use("/", function(req, rsp, next) {
        "use strict";
        if (req.url === "/") {
            rsp.writeHead(303, {
                "Location" : "/ui-new/",
                "Content-Length" : 0,
                "Content-Type" : "application/json"
            });
            rsp.end();
        } else {
            next();
        }
    })
    .use(function(req, rsp) {
        "use strict";
        rsp.writeHead(404, { "Content-Type" : "text/plain" });
        rsp.end("Invalid URI " + JSON.stringify(req.url) + ". Please refer to documentation at http://wiki.yandex-team.ru/YT/ to learn more about HTTP API.");
    })
    .listen(config.port, config.address, function() {
        "use strict";
        logger.info("Worker is listening", {
            wid : cluster.worker.id,
            pid : process.pid
        });
        process.send({ type : "alive" });
    })
    .on("close", violentlyDie);

