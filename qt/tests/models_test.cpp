#include <iostream>
#include <utility>
#include <string>
#include <vector>
#include <chat/text.hpp>

#include <QCoreApplication>

#include "conversation_model.hpp"
#include "message_model.hpp"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    for (auto const& name : std::vector<std::string>{"ASCII", "中文", "normal space", "dot.name", "dash-name", "under_score",
        "r(.*)[z]\\_'", "Alice Bob", "张 三", "Alice\u00a0Bob", "张\u3000三", std::string(64, 'x')})
    {
        if (!chat::valid_username(name)) { return 1; }
    }
    for (auto const& name : std::vector<std::string>{"", " \u00a0\u3000", "a@b", std::string("a\0b", 3), "a\u2028b",
        "a\u202eb", "a\u2066b", std::string(65, 'x'), "中中中中中中中中中中中中中中中中中中中中中中",
        " Alice", "Alice ", " 张三 ", "\tAlice", "Alice\n", "\u00a0Alice", "Alice\u00a0",
        "\u3000张三", "张三\u3000", std::string("\xc0\x80", 2), std::string("\xe4", 1), std::string("\xed\xa0\x80", 3)})
    {
        if (chat::valid_username(name)) { std::cerr << "FAIL invalid username accepted, bytes=" << name.size() << '\n'; return 1; }
    }
    for (int control = 0; control <= 0x9f; ++control)
    {
        if (control >= 0x20 && control < 0x7f) { continue; }
        auto name = QStringLiteral("a") + QChar(control) + QStringLiteral("b");
        if (chat::valid_username(name.toUtf8().toStdString())) { return 1; }
    }
    for (auto codepoint : {0x0009, 0x000a, 0x000b, 0x000c, 0x000d, 0x0020, 0x0085, 0x00a0, 0x1680,
        0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a,
        0x2028, 0x2029, 0x202f, 0x205f, 0x3000})
    {
        auto const space = QString(QChar(codepoint)).toUtf8().toStdString();
        bool const allowed_inside = codepoint == 0x0020 || codepoint == 0x00a0 || codepoint == 0x1680 ||
            (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint >= 0x202f;
        if (chat::valid_username(space + "Alice") || chat::valid_username("Alice" + space) ||
            chat::valid_username("Alice" + space + "Bob") != allowed_inside) { return 1; }
    }
    if (!chat::valid_group_title("  群 名  ") || !chat::valid_group_title(std::string(256, 'x')) ||
        chat::valid_group_title(" \u00a0\u3000") || chat::valid_group_title(std::string(257, 'x')) ||
        chat::valid_group_title(std::string("a\0b", 3))) { return 1; }
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
    if (!second.data(message_model::read_role).toBool() || second.data(message_model::read_count_role).isValid())
    {
        return 1;
    }
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
    {
        message_model late_reply;
        late_reply.reset(1);
        late_reply.merge_messages({deleted});
        if (!late_reply.add_message(outgoing) ||
            !late_reply.index(1, 0).data(message_model::reply_text_role).toString().contains(QStringLiteral("消息已删除")))
        {
            std::cerr << "FAIL late reply resurrects deleted quoted text\n";
            return 1;
        }
        late_reply.reset(1);
        late_reply.merge_messages({outgoing, deleted});
        if (!late_reply.index(1, 0).data(message_model::reply_text_role).toString().contains(QStringLiteral("消息已删除")))
        {
            std::cerr << "FAIL unordered history page resurrects deleted quoted text\n";
            return 1;
        }
        late_reply.reset(1);
        late_reply.merge_messages({edited});
        if (!late_reply.add_message(outgoing) ||
            !late_reply.index(1, 0).data(message_model::reply_text_role).toString().contains(edited.text))
        {
            std::cerr << "FAIL late reply restores an older edit of quoted text\n";
            return 1;
        }
    }
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
    messages.reset(1);
    messages.merge_messages({incoming});
    first = messages.index(0, 0);
    QList<reaction_data> initial_reactions{{QStringLiteral("👍"), {1, 2}}};
    if (!messages.set_reactions(incoming.id, 2, std::move(initial_reactions)) ||
        first.data(message_model::own_reaction_role).toString() != QStringLiteral("👍") ||
        first.data(message_model::reactions_role).value<QList<reaction_data>>().front().users.size() != 2) { return 1; }
    messages.set_reactions(incoming.id, 1, {{QStringLiteral("❤️"), {1}}});
    auto stale_edit = incoming;
    stale_edit.edited_at = 100;
    stale_edit.text = QStringLiteral("edited alongside reaction");
    messages.update_message(stale_edit);
    if (first.data(message_model::own_reaction_role).toString() != QStringLiteral("👍") ||
        first.data(message_model::text_role).toString() != stale_edit.text) { return 1; }
    auto newer_history = incoming;
    newer_history.reaction_revision = 3;
    newer_history.reactions = {{QStringLiteral("😂"), {2}}};
    messages.merge_messages({newer_history});
    if (!first.data(message_model::own_reaction_role).toString().isEmpty() ||
        first.data(message_model::reactions_role).value<QList<reaction_data>>().front().emoji != QStringLiteral("😂")) { return 1; }
    messages.set_reactions(incoming.id, 4, {});
    messages.set_reactions(incoming.id, 3, newer_history.reactions);
    if (!first.data(message_model::reactions_role).value<QList<reaction_data>>().isEmpty()) { return 1; }
    newer_history.deleted = true;
    newer_history.reaction_revision = 5;
    newer_history.reactions.clear();
    messages.update_message(newer_history);
    messages.set_reactions(incoming.id, 6, {{QStringLiteral("👍"), {1}}});
    if (!first.data(message_model::reactions_role).value<QList<reaction_data>>().isEmpty() ||
        messages.set_reactions(incoming.id + 100, 1, {})) { return 1; }
    std::cout << "PASS Qt reaction snapshots, clear, edited/deleted messages and stale revision protection\n";
    message_model mention_model;
    mention_model.set_self_user(1);
    mention_model.reset(1, true);
    if (mention_model.can_manage_group()) { return 1; }
    mention_model.set_members({{1, "owner", chat::member_role::owner, {}}});
    if (!mention_model.can_manage_group()) { return 1; }
    mention_model.set_members({{1, "member", chat::member_role::member, {}}});
    if (mention_model.can_manage_group()) { return 1; }
    mention_model.set_members({{1, "admin", chat::member_role::admin, {}}});
    if (!mention_model.can_manage_group()) { return 1; }
    auto mentioned = incoming;
    mentioned.text = QStringLiteral("@自己");
    mentioned.mentions = {{1, QStringLiteral("自己")}};
    mention_model.add_message(mentioned);
    auto const mention_index = mention_model.index(0, 0);
    if (!mention_index.data(message_model::mentioned_role).toBool() ||
        mention_index.data(message_model::mentions_role).value<QList<mention_data>>().size() != 1) { return 1; }
    auto mention_edit = mentioned;
    mention_edit.edited_at = 200;
    mention_edit.mentions.clear();
    mention_model.update_message(mention_edit);
    mention_model.merge_messages({mentioned});
    if (mention_index.data(message_model::mentioned_role).toBool() ||
        !mention_index.data(message_model::mentions_role).value<QList<mention_data>>().isEmpty()) { return 1; }
    mentioned.edited_at = 300;
    mention_model.update_message(mentioned);
    mentioned.deleted = true;
    mentioned.mentions.clear();
    mention_model.update_message(mentioned);
    if (mention_index.data(message_model::mentioned_role).toBool() ||
        !mention_index.data(message_model::mentions_role).value<QList<mention_data>>().isEmpty()) { return 1; }
    std::cout << "PASS Qt mention identity, stale history, edit replacement and deletion\n";
    conversation_data direct;
    direct.id = 1;
    direct.user = 2;
    conversation_data group;
    group.id = 2;
    group.group = true;
    group.can_send = true;
    group.user = 0;
    group.muted = true;
    group.pinned = true;
    group.pinned_message = {10, "author", "pinned"};
    group.announcement = QStringLiteral("当前群公告\n<纯文本>");
    group.join_approval = true;
    conversations.set_conversations({direct, group});
    if (conversations.conversation_at(conversations.index_for_conversation(1))->can_send ||
        !conversations.conversation_at(conversations.index_for_conversation(2))->can_send) { return 1; }
    direct.can_send = true;
    conversations.set_conversations({direct, group});
    if (!conversations.conversation_at(conversations.index_for_conversation(1))->can_send) { return 1; }
    direct.can_send = false;
    conversations.set_conversations({direct, group});
    if (conversations.conversation_at(conversations.index_for_conversation(1))->can_send) { return 1; }
    if (conversations.conversation_at(conversations.index_for_conversation(2))->announcement != group.announcement ||
        !conversations.conversation_at(conversations.index_for_conversation(1))->announcement.isEmpty() ||
        !conversations.conversation_at(conversations.index_for_conversation(2))->join_approval ||
        conversations.conversation_at(conversations.index_for_conversation(1))->join_approval) { return 1; }
    if (conversations.index_for_conversation(2).data(conversation_model::pinned_message_role).value<quoted_message_data>().id != 10 ||
        conversations.index_for_conversation(1).data(conversation_model::pinned_message_role).value<quoted_message_data>().id != 0) { return 1; }
    conversations.set_online(2, true);
    conversations.set_muted(1, true);
    conversations.set_pinned(1, true);
    if (!conversations.index_for_conversation(1).data(conversation_model::pinned_role).toBool() ||
        !conversations.index_for_conversation(2).data(conversation_model::pinned_role).toBool() ||
        conversations.index_for_conversation(1).row() != 0) { return 1; }
    conversations.set_pinned(2, false);
    if (conversations.index_for_conversation(2).data(conversation_model::pinned_role).toBool()) { return 1; }
    if (!conversations.index_for_conversation(1).data(conversation_model::muted_role).toBool() ||
        !conversations.index_for_conversation(2).data(conversation_model::muted_role).toBool() ||
        conversations.conversation_at(conversations.index_for_conversation(1))->unread != direct.unread) { return 1; }
    conversations.set_muted(2, false);
    if (conversations.index_for_conversation(2).data(conversation_model::muted_role).toBool()) { return 1; }
    conversations.set_conversations({direct, group});
    if (conversations.index_for_conversation(1).data(conversation_model::pinned_role).toBool() ||
        !conversations.index_for_conversation(2).data(conversation_model::pinned_role).toBool()) { return 1; }
    if (conversations.index_for_conversation(1).data(conversation_model::muted_role).toBool() ||
        !conversations.index_for_conversation(2).data(conversation_model::muted_role).toBool()) { return 1; }
    conversations.set_online(2, true);
    if (!conversations.index_for_conversation(1).data(conversation_model::online_role).toBool() ||
        conversations.index_for_conversation(2).data(conversation_model::online_role).toBool())
    {
        return 1;
    }
    std::cout << "PASS Qt identity, group read count/members, leave/rejoin, pagination, read positions and presence\n";
    return 0;
}
