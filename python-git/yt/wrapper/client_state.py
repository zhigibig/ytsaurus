from .system_random import SystemRandom

import random

class ClientState(object):
    def __init__(self):
        self.COMMAND_PARAMS = {
            "transaction_id": "0-0-0-0",
            "ping_ancestor_transactions": False,
            "retry": False
        }
        self._ENABLE_READ_TABLE_CHAOS_MONKEY = False
        self._ENABLE_HTTP_CHAOS_MONKEY = False
        self._ENABLE_HEAVY_REQUEST_CHAOS_MONKEY = False

        self._transaction_stack = None
        self._driver = None
        self._requests_session = None
        self._heavy_proxy_provider = None

        # socket.getfqdn can be slow so client fqdn is cached.
        self._fqdn = None

        # Token can be received from oauth server, in this case we do not want to
        # request on each request to YT.
        self._token = None
        self._token_cached = False

        # Cache for API version (to check it only once).
        self._api_version = None
        self._commands = None
        self._is_testing_mode = None

        # Cache for local_mode_fqdn (used to detect local mode in client).
        self._local_mode_fqdn = None

        self._random_generator = SystemRandom()

    def init_pseudo_random_generator(self):
        """Changes client random generator to pseudo random generator,
           initialized with seed from system generator.
        """
        # This implementation works incorrectky if process forked.
        seed = random.SystemRandom().randint(0, 2**63)
        self._random_generator = random.random()
        self._random_generator.seed(seed)
