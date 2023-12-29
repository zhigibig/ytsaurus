PY3_LIBRARY()

PEERDIR(
    yt/yt/tools/record_codegen/helpers
    library/python/resource
    contrib/python/dacite
    contrib/python/pyaml
)

PY_SRCS(
    __init__.py
)

RESOURCE_FILES(
    yt/yt/ytlib/sequoia_client/records/child_node.yaml
    yt/yt/ytlib/sequoia_client/records/chunk_replicas.yaml
    yt/yt/ytlib/sequoia_client/records/location_replicas.yaml
    yt/yt/ytlib/sequoia_client/records/node_id_to_path.yaml
    yt/yt/ytlib/sequoia_client/records/path_to_node_id.yaml
)

END()
