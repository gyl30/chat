#include <iostream>

#include <QCoreApplication>

#include "conversation_model.hpp"
#include "message_model.hpp"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    message_model messages;
    messages.set_self_user(1);
    messages.reset(1, true);
    message_data incoming;
    incoming.id = 2147483648LL;
    incoming.conversation = 1;
    incoming.from = 2;
    incoming.username = QStringLiteral("成员二");
    incoming.text = QStringLiteral("群消息");
    auto outgoing = incoming;
    outgoing.id++;
    outgoing.from = 1;
    outgoing.username = QStringLiteral("自己");
    outgoing.reply = {incoming.id, incoming.username, incoming.text};
    messages.merge_messages({outgoing, incoming});
    auto first = messages.index(0, 0);
    auto second = messages.index(1, 0);
    if (first.data(message_model::outgoing_role).toBool() || !second.data(message_model::outgoing_role).toBool() ||
        first.data(message_model::sender_name_role).toString() != incoming.username || messages.add_message(incoming))
    {
        return 1;
    }
    if (second.data(message_model::reply_id_role).toLongLong() != incoming.id ||
        !second.data(message_model::reply_text_role).toString().contains(incoming.text))
    {
        return 1;
    }
    messages.set_read_positions({{1, 0}, {2, outgoing.id}, {3, 0}});
    messages.set_members({{1, "self", {}, {}}, {2, "second", {}, {}}, {3, "third", {}, {}}});
    if (!second.data(message_model::read_role).toBool() || second.data(message_model::read_count_role).toInt() != 1 ||
        messages.read_members(outgoing.id).size() != 1 || messages.read_members(outgoing.id).front().id != 2)
    {
        return 1;
    }
    messages.set_read_message(3, outgoing.id);
    if (!second.data(message_model::read_role).toBool() || second.data(message_model::read_count_role).toInt() != 2 ||
        messages.read_members(outgoing.id).size() != 2)
    {
        return 1;
    }
    messages.set_read_message(3, incoming.id);
    if (!second.data(message_model::read_role).toBool())
    {
        return 1;
    }
    messages.set_members({{1, "self", {}, {}}, {3, "third", {}, {}}});
    if (second.data(message_model::read_count_role).toInt() != 1 || messages.read_members(outgoing.id).size() != 1 ||
        messages.read_members(outgoing.id).front().id != 3)
    {
        return 1;
    }
    messages.set_read_positions({{1, outgoing.id}, {4, 0}});
    messages.set_members({{1, "self", {}, {}}, {4, "new member", {}, {}}});
    if (second.data(message_model::read_role).toBool() || second.data(message_model::read_count_role).toInt() != 0 ||
        !messages.read_members(outgoing.id).isEmpty())
    {
        return 1;
    }
    messages.set_read_positions({{1, outgoing.id}});
    if (second.data(message_model::read_role).toBool())
    {
        return 1;
    }
    auto foreign = incoming;
    foreign.conversation = 2;
    foreign.id += 10;
    if (messages.add_message(foreign) || messages.rowCount() != 2)
    {
        return 1;
    }
    auto edited = incoming;
    edited.text = QStringLiteral("新内容");
    edited.edited_at = 100;
    messages.update_message(edited);
    messages.merge_messages({incoming, outgoing});
    if (first.data(message_model::text_role).toString() != edited.text ||
        first.data(message_model::edited_at_role).toLongLong() != 100 ||
        !second.data(message_model::reply_text_role).toString().contains(edited.text))
    {
        return 1;
    }
    auto deleted = edited;
    deleted.deleted = true;
    deleted.text.clear();
    messages.update_message(deleted);
    edited.edited_at = 200;
    messages.update_message(edited);
    messages.merge_messages({incoming, outgoing});
    if (!first.data(message_model::deleted_role).toBool() ||
        first.data(message_model::text_role).toString() != QStringLiteral("消息已删除") ||
        !second.data(message_model::reply_text_role).toString().contains(QStringLiteral("消息已删除")))
    {
        return 1;
    }
    messages.reset(1);
    messages.merge_messages({outgoing});
    messages.set_read_positions({{1, outgoing.id}, {2, 0}});
    if (messages.index(0, 0).data(message_model::read_role).toBool() ||
        messages.index(0, 0).data(message_model::read_count_role).isValid() || !messages.read_members(outgoing.id).isEmpty())
    {
        return 1;
    }
    messages.set_read_message(2, outgoing.id);
    if (!messages.index(0, 0).data(message_model::read_role).toBool())
    {
        return 1;
    }
    messages.reset(1, true);
    messages.merge_messages({outgoing});
    messages.set_members({{1, "self", {}, {}}, {2, "second", {}, {}}, {3, "third", {}, {}}});
    messages.set_read_positions({{1, outgoing.id}, {2, 0}, {3, 0}});
    if (messages.index(0, 0).data(message_model::read_count_role).toInt() != 0)
    {
        return 1;
    }
    messages.set_read_message(2, outgoing.id);
    messages.merge_messages({incoming});
    messages.set_read_positions({{1, outgoing.id}, {2, 0}, {3, 0}});
    if (messages.index(0, 0).data(message_model::read_count_role).toInt() != 1 ||
        messages.index(1, 0).data(message_model::read_count_role).toInt() != 1 ||
        messages.read_members(incoming.id).front().id != 2)
    {
        return 1;
    }
    messages.set_read_positions({{1, outgoing.id}, {3, 0}});
    messages.set_members({{1, "self", {}, {}}, {3, "third", {}, {}}});
    messages.set_read_positions({{1, outgoing.id}, {2, 0}, {3, 0}});
    messages.set_members({{1, "self", {}, {}}, {2, "rejoined", {}, {}}, {3, "third", {}, {}}});
    if (messages.index(1, 0).data(message_model::read_count_role).toInt() != 0 ||
        !messages.read_members(outgoing.id).isEmpty())
    {
        return 1;
    }
    messages.set_read_message(2, outgoing.id);
    if (messages.read_members(outgoing.id).front().username != "rejoined")
    {
        return 1;
    }
    messages.set_read_positions({});
    messages.set_read_positions({{1, outgoing.id}, {2, 0}, {3, 0}});
    if (messages.index(1, 0).data(message_model::read_count_role).toInt() != 0 ||
        !messages.read_members(outgoing.id).isEmpty())
    {
        return 1;
    }
    messages.reset(1);
    messages.merge_messages({outgoing});
    messages.set_read_positions({{1, outgoing.id}});
    if (!messages.index(0, 0).data(message_model::read_role).toBool())
    {
        return 1;
    }
    conversation_model conversations;
    conversation_data direct;
    direct.id = 1;
    direct.user = 2;
    conversation_data group;
    group.id = 2;
    group.group = true;
    group.user = 0;
    conversations.set_conversations({direct, group});
    conversations.set_online(2, true);
    if (!conversations.index_for_conversation(1).data(conversation_model::online_role).toBool() ||
        conversations.index_for_conversation(2).data(conversation_model::online_role).toBool())
    {
        return 1;
    }
    std::cout << "PASS Qt identity, group read count/members, leave/rejoin, pagination, read positions and presence\n";
    return 0;
}
