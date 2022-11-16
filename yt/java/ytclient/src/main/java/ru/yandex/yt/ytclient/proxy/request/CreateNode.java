package ru.yandex.yt.ytclient.proxy.request;

import java.util.Map;

import tech.ytsaurus.client.request.ObjectType;
import tech.ytsaurus.core.cypress.CypressNodeType;
import tech.ytsaurus.core.cypress.YPath;
import tech.ytsaurus.ysontree.YTreeNode;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;

/**
 * Request for creating cypress node.
 *
 * @see <a href="https://docs.yandex-team.ru/yt/api/commands#create">
 *     create documentation
 *     </a>
 */
@NonNullApi
@NonNullFields
public class CreateNode extends tech.ytsaurus.client.request.CreateNode.BuilderBase<CreateNode> {
    public CreateNode(tech.ytsaurus.client.request.CreateNode other) {
        super(other.toBuilder());
    }

    public CreateNode(CreateNode other) {
        super(other);
    }

    public CreateNode(String path, ObjectType type) {
        this(YPath.simple(path), type);
    }

    public CreateNode(YPath path, ObjectType type) {
        setPath(path).setType(type);
    }

    public CreateNode(String path, ObjectType type, Map<String, YTreeNode> attributes) {
        this(path, type);
        setAttributes(attributes);
    }

    public CreateNode(YPath path, CypressNodeType type) {
        this(path, ObjectType.from(type));
    }

    public CreateNode(YPath path, CypressNodeType type, Map<String, YTreeNode> attributes) {
        this(path, type);
        setAttributes(attributes);
    }

    @Override
    protected CreateNode self() {
        return this;
    }
}
