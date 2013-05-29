import http_config
import logger
from common import require
from errors import YtResponseError, YtNetworkError, YtTokenError, format_error
import yt.yson as yson

import os
import string
import time
import httplib
import simplejson as json

# We cannot use requests.HTTPError in module namespace because of conflict with python3 http library
from requests import HTTPError, ConnectionError, Timeout
NETWORK_ERRORS = (HTTPError, ConnectionError, Timeout, httplib.IncompleteRead, YtResponseError, YtNetworkError)

class Response(object):
    def __init__(self, http_response):
        self.http_response = http_response
        self._return_code_processed = False

    def error(self):
        self._process_return_code()
        return self._error

    def is_ok(self):
        self._process_return_code()
        return not hasattr(self, "_error")

    def is_json(self):
        return self.http_response.headers.get("content-type") == "application/json"

    def is_yson(self):
        content_type = self.http_response.headers.get("content-type")
        return isinstance(content_type, str) and content_type.startswith("application/x-yt-yson")

    def json(self):
        return self.http_response.json()

    def yson(self):
        return yson.loads(self.content())

    def content(self):
        return self.http_response.content

    def _process_return_code(self):
        if self._return_code_processed:
            return

        if not str(self.http_response.status_code).startswith("2"):
            # 401 is case of incorrect token
            if self.http_response.status_code == 401:
                raise YtTokenError(
                    "Your authentication token was rejected by the server (X-YT-Request-ID: %s).\n"
                    "Please refer to http://proxy.yt.yandex.net/auth/ for obtaining a valid token or contact us at yt@yandex-team.ru." %
                    self.http_response.headers.get("X-YT-Request-ID", "absent"))
            self._error = format_error(self.http_response.json())
        elif int(self.http_response.headers.get("x-yt-response-code", 0)) != 0:
            self._error = format_error(json.loads(self.http_response.headers["x-yt-error"]))
        self._return_code_processed = True


def get_token():
    token = http_config.TOKEN
    if token is None:
        token_path = os.path.join(os.path.expanduser("~"), ".yt/token")
        if os.path.isfile(token_path):
            token = open(token_path).read().strip()
    if token is not None:
        require(all(c in string.hexdigits for c in token),
                YtTokenError("You have an improper authentication token in ~/.yt_token.\n"
                             "Please refer to http://proxy.yt.yandex.net/auth/ for obtaining a valid token."))
    if not token:
        token = None
    return token

def make_request_with_retries(request, make_retries=False, url="", return_raw_response=False):
    for attempt in xrange(http_config.HTTP_RETRIES_COUNT):
        try:
            response = request()
            is_json = response.is_json() or not str(response.http_response.status_code).startswith("2")
            if not return_raw_response and is_json and not response.content():
                raise YtResponseError(
                        "Response has empty body and JSON content type (Headers: %s)" %
                        repr(response.http_response.headers))
            return response
        except NETWORK_ERRORS as error:
            message =  "HTTP request (%s) has failed with error '%s'" % (url, str(error))
            if make_retries:
                logger.warning("%s. Retrying...", message)
                time.sleep(http_config.HTTP_RETRY_TIMEOUT)
            else:
                if not isinstance(error, YtResponseError):
                    raise YtNetworkError("Connection to URL %s has failed with error %s", url, str(error))
                else:
                    raise

