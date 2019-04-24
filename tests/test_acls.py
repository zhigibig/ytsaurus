from .conftest import ZERO_RESOURCE_REQUESTS

from yp.common import (
    YP_NO_SUCH_OBJECT_ERROR_CODE,
    YpAuthorizationError,
    YpClientError,
    YpNoSuchObjectError,
    YtResponseError,
    validate_error_recursively,
)

from yt.environment.helpers import assert_items_equal

from yt.packages.six.moves import xrange

import pytest


@pytest.mark.usefixtures("yp_env")
class TestAcls(object):
    def test_owner(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        yp_client.create_object("user", attributes={"meta": {"id": "u2"}})

        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            id = yp_client1.create_object("pod_set")
            yp_client1.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

        with yp_env.yp_instance.create_client(config={"user": "u2"}) as yp_client2:
            yp_env.sync_access_control()

            with pytest.raises(YpAuthorizationError):
                yp_client2.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

    def test_groups_immediate(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        yp_client.create_object("group", attributes={"meta": {"id": "g"}})

        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            id = yp_client.create_object("pod_set", attributes={
                "meta": {
                    "acl": [
                        {"action": "allow", "permissions": ["write"], "subjects": ["g"]}
                    ]
                }
            })
            with pytest.raises(YpAuthorizationError):
                yp_client1.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

            yp_client.update_object("group", "g", set_updates=[{"path": "/spec/members", "value": ["u1"]}])
            yp_env.sync_access_control()

            yp_client1.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

    def test_groups_recursive(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        yp_client.create_object("group", attributes={"meta": {"id": "g1"}})
        yp_client.create_object("group", attributes={"meta": {"id": "g2"}})
        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            id = yp_client.create_object("pod_set", attributes={
                "meta": {
                    "acl": [
                        {"action": "allow", "permissions": ["write"], "subjects": ["g1"]}
                    ]
                }
            })
            with pytest.raises(YpAuthorizationError):
                yp_client1.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

            yp_client.update_object("group", "g1", set_updates=[{"path": "/spec/members", "value": ["g2"]}])
            yp_client.update_object("group", "g2", set_updates=[{"path": "/spec/members", "value": ["u1"]}])
            yp_env.sync_access_control()

            yp_client1.update_object("pod_set", id, set_updates=[{"path": "/labels/a", "value": "b"}])

    def test_inherit_acl(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            pod_set_id = yp_client1.create_object("pod_set")
            pod_id = yp_client1.create_object("pod", attributes={"meta": {"pod_set_id": pod_set_id, "acl": []}})
            yp_client1.update_object("pod", pod_id, set_updates=[{"path": "/labels/a", "value": "b"}])

            yp_client.update_object("pod", pod_id, set_updates=[{"path": "/meta/inherit_acl", "value": False}])
            with pytest.raises(YpAuthorizationError):
                yp_client1.update_object("pod", pod_id, set_updates=[{"path": "/labels/a", "value": "b"}])

    def test_endpoint_inherits_from_endpoint_set(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            endpoint_set_id = yp_client.create_object("endpoint_set")
            endpoint_id = yp_client.create_object("endpoint", attributes={
                "meta": {
                    "endpoint_set_id": endpoint_set_id
                }
            })

            with pytest.raises(YpAuthorizationError):
                yp_client1.update_object("endpoint", endpoint_id, set_updates=[{"path": "/labels/a", "value": "b"}])
            yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[
                {
                    "path": "/meta/acl",
                    "value": [
                        {"action": "allow", "permissions": ["write"], "subjects": ["u1"]}
                    ]
                }])
            yp_client1.update_object("endpoint", endpoint_id, set_updates=[{"path": "/labels/a", "value": "b"}])

    def test_check_permissions(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u"}})
        yp_env.sync_access_control()

        endpoint_set_id = yp_client.create_object("endpoint_set", attributes={
                "meta": {
                    "acl": [
                        {"action": "allow", "subjects": ["u"], "permissions": ["write"]}
                    ]
                }
            })

        assert \
            yp_client.check_object_permissions([
                {"object_type": "endpoint_set", "object_id": endpoint_set_id, "subject_id": "u", "permission": "read"},
                {"object_type": "endpoint_set", "object_id": endpoint_set_id, "subject_id": "u", "permission": "write"},
                {"object_type": "endpoint_set", "object_id": endpoint_set_id, "subject_id": "u", "permission": "ssh_access"}
            ]) == \
            [
                {"action": "allow", "subject_id": "everyone", "object_type": "schema", "object_id": "endpoint_set"},
                {"action": "allow", "subject_id": "u", "object_type": "endpoint_set", "object_id": endpoint_set_id},
                {"action": "deny"}
            ]

    def test_create_at_schema(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            yp_client.update_object("schema", "endpoint_set", set_updates=[{
                "path": "/meta/acl/end",
                "value": {"action": "deny", "permissions": ["create"], "subjects": ["u1"]}
            }])

            with pytest.raises(YpAuthorizationError):
                yp_client1.create_object("endpoint_set")

            yp_client.update_object("schema", "endpoint_set", remove_updates=[{
                "path": "/meta/acl/-1"
            }])

            yp_client1.create_object("endpoint_set")

    def test_create_requires_write_at_parent(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})

        with yp_env.yp_instance.create_client(config={"user": "u1"}) as yp_client1:
            yp_env.sync_access_control()

            endpoint_set_id = yp_client1.create_object("endpoint_set")

            yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[{
                "path": "/meta/acl",
                "value": [{"action": "deny", "permissions": ["write"], "subjects": ["u1"]}]
            }])

            with pytest.raises(YpAuthorizationError):
                yp_client1.create_object("endpoint", attributes={"meta": {"endpoint_set_id": endpoint_set_id}})

            yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[{
                "path": "/meta/acl",
                "value": [{"action": "allow", "permissions": ["write"], "subjects": ["u1"]}]
            }])

            yp_client1.create_object("endpoint", attributes={"meta": {"endpoint_set_id": endpoint_set_id}})

    def test_get_object_access_allowed_for(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u1"}})
        yp_client.create_object("user", attributes={"meta": {"id": "u2"}})
        yp_client.create_object("user", attributes={"meta": {"id": "u3"}})

        yp_client.create_object("group", attributes={"meta": {"id": "g1"}, "spec": {"members": ["u1", "u2"]}})
        yp_client.create_object("group", attributes={"meta": {"id": "g2"}, "spec": {"members": ["u2", "u3"]}})
        yp_client.create_object("group", attributes={"meta": {"id": "g3"}, "spec": {"members": ["g1", "g2"]}})

        yp_env.sync_access_control()

        endpoint_set_id = yp_client.create_object("endpoint_set", attributes={"meta": {"inherit_acl": False}})

        assert_items_equal(
            yp_client.get_object_access_allowed_for([
                {"object_id": endpoint_set_id, "object_type": "endpoint_set", "permission": "read"}
            ])[0]["user_ids"],
            ["root"])

        yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[{
                "path": "/meta/acl",
                "value": [{"action": "allow", "permissions": ["read"], "subjects": ["u1", "u2"]}]
            }])

        assert_items_equal(
            yp_client.get_object_access_allowed_for([
                {"object_id": endpoint_set_id, "object_type": "endpoint_set", "permission": "read"}
            ])[0]["user_ids"],
            ["root", "u1", "u2"])

        yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[{
                "path": "/meta/acl",
                "value": [{"action": "deny", "permissions": ["read"], "subjects": ["root"]}]
            }])

        assert_items_equal(
            yp_client.get_object_access_allowed_for([
                {"object_id": endpoint_set_id, "object_type": "endpoint_set", "permission": "read"}
            ])[0]["user_ids"],
            ["root"])

        yp_client.update_object("endpoint_set", endpoint_set_id, set_updates=[{
                "path": "/meta/acl",
                "value": [
                    {"action": "allow", "permissions": ["read"], "subjects": ["g3"]},
                    {"action": "deny", "permissions": ["read"], "subjects": ["g2"]}
                ]
            }])

        assert_items_equal(
            yp_client.get_object_access_allowed_for([
                {"object_id": endpoint_set_id, "object_type": "endpoint_set", "permission": "read"}
            ])[0]["user_ids"],
            ["root", "u1"])

    def test_get_user_access_allowed_to(self, yp_env):
        yp_client = yp_env.yp_client

        # Empty subrequests.
        assert_items_equal(yp_client.get_user_access_allowed_to([]), [])

        yp_client.create_object("user", attributes=dict(meta=dict(id="u1")))

        def create_network_project(acl, inherit_acl):
            return yp_client.create_object(
                "network_project",
                attributes=dict(
                    meta=dict(acl=acl, inherit_acl=inherit_acl),
                    spec=dict(project_id=42),
                ),
            )

        network_project_id1 = create_network_project(acl=[], inherit_acl=False)
        yp_env.sync_access_control()

        # User is not granted the access. Superuser is always granted the access.
        # Different users / permissions in the one request.
        assert_items_equal(
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="network_project",
                    permission="read",
                ),
                dict(
                    user_id="root",
                    object_type="network_project",
                    permission="write",
                ),
                dict(
                    user_id="root",
                    object_type="network_project",
                    permission="read",
                ),
            ]),
            [
                dict(object_ids=[]),
                dict(object_ids=[network_project_id1]),
                dict(object_ids=[network_project_id1]),
            ],
        )

        network_project_id2 = create_network_project(
            acl=[
                dict(action="allow", subjects=["u1"], permissions=["read", "write"]),
            ],
            inherit_acl=True,
        )
        yp_env.sync_access_control()

        # User is granted the access.
        assert_items_equal(
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="network_project",
                    permission="read",
                )
            ]),
            [
                dict(object_ids=[network_project_id2]),
            ],
        )

        # Method is not supported for the 'pod' object type.
        try:
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="pod",
                    permission="read",
                )
            ])
        except YtResponseError as error:
            assert error.contains_code(103) # No such method.

        network_project_id3 = create_network_project(
            acl=[
                dict(action="allow", subjects=["u1"], permissions=["read", "write"]),
            ],
            inherit_acl=True,
        )
        yp_env.sync_access_control()

        # Several objects in the response.
        response = yp_client.get_user_access_allowed_to([
            dict(
                user_id="u1",
                object_type="network_project",
                permission="read",
            ),
            dict(
                user_id="u1",
                object_type="network_project",
                permission="write",
            ),
        ])
        assert len(response) == 2
        assert set(response[0]["object_ids"]) == set([network_project_id2, network_project_id3])
        assert set(response[1]["object_ids"]) == set([network_project_id2, network_project_id3])

        # Nonexistant user.
        assert_items_equal(
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="abracadabra",
                    object_type="network_project",
                    permission="read",
                ),
            ]),
            [
                dict(object_ids=[]),
            ],
        )

        # Nonexistant object type.
        with pytest.raises(YpClientError):
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="abracadabra",
                    permission="read",
                ),
            ])

        # Nonexistant permission.
        with pytest.raises(YpClientError):
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="network_project",
                    permission="abracadabra",
                ),
            ])

        yp_client.create_object("group", attributes=dict(
            meta=dict(id="g1"),
            spec=dict(members=["u1"]),
        ))

        yp_client.update_object("network_project", network_project_id2, set_updates=[
            dict(
                path="/meta/acl",
                value=[
                    dict(action="allow", permissions=["write"], subjects=["u1"]),
                    dict(action="deny", permissions=["write"], subjects=["g1"]),
                ]
            ),
        ])
        yp_env.sync_access_control()

        # User is not granted the access because of group ace with action = "deny".
        assert_items_equal(
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id="u1",
                    object_type="network_project",
                    permission="write",
                ),
            ]),
            [
                dict(object_ids=[network_project_id3]),
            ],
        )

        object_count = 100
        object_ids = []
        for _ in xrange(object_count):
            object_ids.append(
                yp_client.create_object(
                    "account",
                    attributes=dict(meta=dict(acl=[
                        dict(
                            action="allow",
                            permissions=["read"],
                            subjects=["u1"],
                        )
                    ])),
                )
            )

        yp_env.sync_access_control()

        assert_items_equal(
            set(
                yp_client.get_user_access_allowed_to([
                    dict(
                        user_id="u1",
                        object_type="account",
                        permission="read",
                    ),
                ])[0]["object_ids"]
            ),
            set(object_ids),
        )

    def test_only_superuser_can_force_assign_pod1(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u"}})

        yp_env.sync_access_control()

        node_id = yp_client.create_object("node")
        pod_set_id = yp_client.create_object("pod_set", attributes={
                "meta": {
                    "acl": [{"action": "allow", "permissions": ["write"], "subjects": ["u"]}]
                }
            })

        with yp_env.yp_instance.create_client(config={"user": "u"}) as yp_client1:
            def try_create():
                yp_client1.create_object("pod", attributes={
                    "meta": {
                        "pod_set_id": pod_set_id
                    },
                    "spec": {
                        "resource_requests": ZERO_RESOURCE_REQUESTS,
                        "node_id": node_id
                    }
                })

            with pytest.raises(YpAuthorizationError):
                try_create()

            yp_client.update_object("group", "superusers", set_updates=[
                {"path": "/spec/members", "value": ["u"]}
            ])

            yp_env.sync_access_control()

            try_create()

    def test_only_superuser_can_force_assign_pod2(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u"}})

        yp_env.sync_access_control()

        node_id = yp_client.create_object("node")
        pod_set_id = yp_client.create_object("pod_set", attributes={
                "meta": {
                    "acl": [{"action": "allow", "permissions": ["write"], "subjects": ["u"]}]
                }
            })
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
                    "resource_requests": ZERO_RESOURCE_REQUESTS
                }
            })

        with yp_env.yp_instance.create_client(config={"user": "u"}) as yp_client1:
            def try_update():
                yp_client1.update_object("pod", pod_id, set_updates=[
                    {"path": "/spec/node_id", "value": node_id}
                ])

            with pytest.raises(YpAuthorizationError):
                try_update()

            yp_client.update_object("group", "superusers", set_updates=[
                {"path": "/spec/members", "value": ["u"]}
            ])

            yp_env.sync_access_control()

            try_update()

    def test_only_superuser_can_request_subnet_without_network_project(self, yp_env):
        yp_client = yp_env.yp_client

        yp_client.create_object("user", attributes={"meta": {"id": "u"}})

        yp_env.sync_access_control()

        node_id = yp_client.create_object("node")
        pod_set_id = yp_client.create_object("pod_set", attributes={
                "meta": {
                    "acl": [{"action": "allow", "permissions": ["write"], "subjects": ["u"]}]
                }
            })

        with yp_env.yp_instance.create_client(config={"user": "u"}) as yp_client1:
            def try_create():
                yp_client1.create_object("pod", attributes={
                    "meta": {
                        "pod_set_id": pod_set_id
                    },
                    "spec": {
                        "resource_requests": ZERO_RESOURCE_REQUESTS,
                        "ip6_subnet_requests": [
                            {"vlan_id": "somevlan"}
                        ]
                    }
                })

            with pytest.raises(YpAuthorizationError):
                try_create()

            yp_client.update_object("group", "superusers", set_updates=[
                {"path": "/spec/members", "value": ["u"]}
            ])

            yp_env.sync_access_control()

            try_create()

    def test_nonexistant_subject_in_acl(self, yp_env):
        yp_client = yp_env.yp_client

        def with_error(callback, subject_id):
            def validate_error(error):
                return int(error.get("code", 0)) == YP_NO_SUCH_OBJECT_ERROR_CODE and \
                    error.get("attributes", {}).get("object_id", None) == subject_id
            with pytest.raises(YpNoSuchObjectError) as exception_info:
                callback()
            assert validate_error_recursively(exception_info.value.error, validate_error)

        subject_name = "subject"
        acl = [
            dict(
                action="allow",
                subjects=["root", subject_name],
                permissions=["read"]
            ),
            dict(
                action="allow",
                subjects=["superusers"],
                permissions=["write"]
            )
        ]

        with_error(
            lambda: yp_client.create_object(
                "pod_set",
                attributes=dict(meta=dict(acl=acl)),
            ),
            subject_name
        )

        pod_set_id = yp_client.create_object("pod_set")

        with_error(
            lambda: yp_client.update_object(
                "pod_set",
                pod_set_id,
                set_updates=[
                    dict(
                        path="/meta/acl",
                        value=acl,
                    )
                ]
            ),
            subject_name
        )

        # Add ace with existant subject when acl already contains non-existant subject.
        subject_name2 = "subject2"
        yp_client.create_object("user", attributes=dict(meta=dict(id=subject_name)))
        yp_client.create_object("user", attributes=dict(meta=dict(id=subject_name2)))
        yp_env.sync_access_control()

        yp_client.update_object(
            "pod_set",
            pod_set_id,
            set_updates=[
                dict(
                    path="/meta/acl",
                    value=acl,
                )
            ]
        )

        yp_client.remove_object("user", subject_name)
        yp_env.sync_access_control()

        yp_client.update_object(
            "pod_set",
            pod_set_id,
            set_updates=[
                dict(
                    path="/meta/acl/end",
                    value=dict(
                        action="allow",
                        permissions=["read"],
                        subjects=[subject_name2]
                    )
                )
            ]
        )


@pytest.mark.usefixtures("yp_env_configurable")
class TestApiGetUserAccessAllowedTo(object):
    YP_MASTER_CONFIG = dict(
        access_control_manager = dict(
            cluster_state_allowed_object_types = ["pod"],
        )
    )

    def test(self, yp_env_configurable):
        yp_client = yp_env_configurable.yp_client

        u1 = yp_client.create_object("user")

        yp_env_configurable.sync_access_control()

        # Method is not supported for the 'network_project' object type.
        try:
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id=u1,
                    object_type="network_project",
                    permission="create",
                )
            ])
        except YtResponseError as error:
            assert error.contains_code(103) # No such method.

        pod_set_id = yp_client.create_object("pod_set", attributes=dict(meta=dict(
            acl=[
                dict(
                    action="allow",
                    subjects=[u1],
                    permissions=["write"]
                )
            ]
        )))

        # Pod with allowed 'write' permission due to the parent pod set acl.
        pod1 = yp_client.create_object("pod", attributes=dict(meta=dict(
            pod_set_id=pod_set_id,
            inherit_acl=True,
            acl=[],
        )))

        # Pod with denied 'write' permission due to the empty acl list and inherit_acl == False.
        pod2 = yp_client.create_object("pod", attributes=dict(meta=dict(
            pod_set_id=pod_set_id,
            inherit_acl=False,
            acl=[],
        )))

        # Pod with denied 'write' permission due to the ace with action == 'deny', despite of the
        # inherit_acl == True and allowed 'write' permission due to the parent pod set acl.
        pod3 = yp_client.create_object("pod", attributes=dict(meta=dict(
            pod_set_id=pod_set_id,
            inherit_acl=True,
            acl=[
                dict(
                    action="deny",
                    subjects=[u1],
                    permissions=["write"],
                )
            ]
        )))

        yp_env_configurable.sync_access_control()

        assert_items_equal(
            yp_client.get_user_access_allowed_to([
                dict(
                    user_id=u1,
                    object_type="pod",
                    permission="write",
                ),
            ]),
            [
                dict(object_ids=[pod1]),
            ],
        )
