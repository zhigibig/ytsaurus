QUEUE_TABLE_SCHEMA = [
    {"name": "cluster", "type": "string", "sort_order": "ascending"},
    {"name": "path", "type": "string", "sort_order": "ascending"},
    {"name": "row_revision", "type": "uint64"},
    {"name": "revision", "type": "uint64"},
    {"name": "object_type", "type": "string"},
    {"name": "dynamic", "type": "boolean"},
    {"name": "sorted", "type": "boolean"},
    {"name": "auto_trim_config", "type": "any"},
    {"name": "queue_agent_stage", "type": "string"},
    {"name": "synchronization_error", "type": "any"},
]

CONSUMER_TABLE_SCHEMA = [
    {"name": "cluster", "type": "string", "sort_order": "ascending"},
    {"name": "path", "type": "string", "sort_order": "ascending"},
    {"name": "row_revision", "type": "uint64"},
    {"name": "revision", "type": "uint64"},
    {"name": "object_type", "type": "string"},
    {"name": "treat_as_queue_consumer", "type": "boolean"},
    {"name": "schema", "type": "any"},
    {"name": "queue_agent_stage", "type": "string"},
    {"name": "synchronization_error", "type": "any"},
]

REGISTRATION_TABLE_SCHEMA = [
    {"name": "queue_cluster", "type": "string", "sort_order": "ascending"},
    {"name": "queue_path", "type": "string", "sort_order": "ascending"},
    {"name": "consumer_cluster", "type": "string", "sort_order": "ascending"},
    {"name": "consumer_path", "type": "string", "sort_order": "ascending"},
    {"name": "vital", "type": "boolean"},
]

DEFAULT_ROOT = "//sys/queue_agents"
DEFAULT_REGISTRATION_TABLE_PATH = DEFAULT_ROOT + "/consumer_registrations"


def create_tables(client, root=DEFAULT_ROOT, registration_table_path=DEFAULT_REGISTRATION_TABLE_PATH,
                  skip_queues=False, skip_consumers=False, create_registration_table=False,
                  queue_table_schema=None, consumer_table_schema=None, registration_table_schema=None):
    queue_table_schema = queue_table_schema or QUEUE_TABLE_SCHEMA
    consumer_table_schema = consumer_table_schema or CONSUMER_TABLE_SCHEMA
    registration_table_schema = registration_table_schema or REGISTRATION_TABLE_SCHEMA
    if not skip_queues:
        client.create("table", root + "/queues", attributes={"dynamic": True, "schema": queue_table_schema})
    if not skip_consumers:
        client.create("table", root + "/consumers", attributes={"dynamic": True, "schema": consumer_table_schema})
    if create_registration_table:
        client.create("table", registration_table_path,
                      attributes={"dynamic": True, "schema": registration_table_schema})
