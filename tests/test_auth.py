from yp.tests.helpers.conftest_auth import (
    BLACKBOX_OAUTH_REQUEST_TEMPLATE,
    BLACKBOX_OAUTH_RESPONSE,
)

from yp.common import YtResponseError

import copy
import pytest


TEST_USER_TICKET = "MyUserTicket"

BLACKBOX_USER_TICKET_REQUEST = "method=user_ticket&user_ticket={}&attributes=1008&format=json".format(
    TEST_USER_TICKET
)

BLACKBOX_USER_TICKET_RESPONSE = {
    "users": [{"login": "root",},],
}


def _create_client(yp_env_auth, config):
    """Overrides the default client config."""
    return yp_env_auth.yp_instance.create_client(config=config)


def _select_objects_succeeds(yp_client):
    assert yp_client.select_objects("pod", selectors=["/meta/id"]) == []


def _select_objects_fails(yp_client):
    with pytest.raises(YtResponseError):
        yp_client.select_objects("pod", selectors=["/meta/id"])


@pytest.mark.usefixtures("yp_env_auth", "set_bb_response")
class TestAuth(object):
    YP_MASTER_CONFIG = {
        "authentication_manager": {"blackbox_ticket_authenticator": {},},
    }

    def test_no_auth(self, yp_env_auth):
        with _create_client(yp_env_auth, {"user": "root"}) as yp_client:
            _select_objects_fails(yp_client)

    def test_good_oauth_token(self, yp_env_auth):
        # the correct token is in the default auth config
        yp_client = yp_env_auth.yp_client
        _select_objects_succeeds(yp_client)

    def test_bad_oauth_token(self, yp_env_auth):
        with _create_client(yp_env_auth, {"token": "BadOauthToken"}) as yp_client:
            _select_objects_fails(yp_client)

    def test_bad_oauth_token_scope(self, yp_env_auth, set_bb_response):
        token = "OtherOauthToken"
        request = BLACKBOX_OAUTH_REQUEST_TEMPLATE.format(token)
        response = copy.deepcopy(BLACKBOX_OAUTH_RESPONSE)
        response["oauth"]["scope"] = "bad:scope"
        set_bb_response(request, response)
        with _create_client(yp_env_auth, {"token": token}) as yp_client:
            _select_objects_fails(yp_client)

    def test_good_user_ticket(self, yp_env_auth, set_bb_response):
        set_bb_response(BLACKBOX_USER_TICKET_REQUEST, BLACKBOX_USER_TICKET_RESPONSE)
        with _create_client(yp_env_auth, {"user_ticket": TEST_USER_TICKET}) as yp_client:
            _select_objects_succeeds(yp_client)
            yp_client.update_user_ticket("BadUserTicket")
            _select_objects_fails(yp_client)
            yp_client.update_user_ticket(TEST_USER_TICKET)
            _select_objects_succeeds(yp_client)

    def test_bad_user_ticket(self, yp_env_auth, set_bb_response):
        set_bb_response(BLACKBOX_USER_TICKET_REQUEST, BLACKBOX_USER_TICKET_RESPONSE)
        with _create_client(yp_env_auth, {"user_ticket": "BadUserTicket"}) as yp_client:
            _select_objects_fails(yp_client)
            yp_client.update_user_ticket(TEST_USER_TICKET)
            _select_objects_succeeds(yp_client)
            yp_client.update_user_ticket("BadUserTicket")
            _select_objects_fails(yp_client)
