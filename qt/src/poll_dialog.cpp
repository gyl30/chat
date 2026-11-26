#include "poll_dialog.hpp"
#include "avatar.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
struct voter_section
{
    QVBoxLayout* people;
    QPushButton* more;
    quint64 request = 0;
    qint64 after = 0;
};
}    // namespace

void show_poll(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("投票结果"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* question = new QLabel;
    question->setWordWrap(true);
    question->setTextFormat(Qt::PlainText);
    auto font = question->font();
    font.setBold(true);
    question->setFont(font);
    layout->addWidget(question);
    auto* status = new QLabel;
    layout->addWidget(status);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);
    dialog.resize(450, 470);
    QHash<qint64, voter_section> sections;
    quint64 query = 0;
    qint64 revision = 0;
    auto reload = [&] { query = client.get_poll(room, message); };
    auto load_people = [&](qint64 choice)
    {
        auto& section = sections[choice];
        section.more->setEnabled(false);
        section.request = client.get_poll_voters(room, message, choice, section.after);
    };
    auto render = [&](const chat::poll_summary& poll)
    {
        if (poll.revision <= revision)
        {
            return;
        }
        revision = poll.revision;
        sections.clear();
        auto* body = new QWidget;
        auto* groups = new QVBoxLayout(body);
        groups->setAlignment(Qt::AlignTop);
        qint64 total = 0;
        for (auto const& option : poll.options)
        {
            total += option.votes;
        }
        status->setText(QStringLiteral("%1 · 共 %2 票").arg(poll.closed ? QStringLiteral("投票已结束") : QStringLiteral("实名单选")).arg(total));
        for (auto const& option : poll.options)
        {
            auto* heading = new QLabel(QStringLiteral("%1%   %2   ·   %3 票")
                                           .arg(total ? qRound(100. * option.votes / total) : 0)
                                           .arg(QString::fromStdString(option.label))
                                           .arg(option.votes));
            heading->setObjectName(QStringLiteral("pollGroup"));
            heading->setFont(font);
            heading->setTextFormat(Qt::PlainText);
            heading->setWordWrap(true);
            groups->addWidget(heading);
            auto* people = new QVBoxLayout;
            groups->addLayout(people);
            auto* more = new QPushButton(QStringLiteral("更多投票成员"));
            more->setVisible(false);
            more->setProperty("choice", qint64(option.position));
            groups->addWidget(more);
            sections.insert(option.position, {people, more});
            QObject::connect(more, &QPushButton::clicked, &dialog, [&, choice = option.position] { load_people(choice); });
            if (option.votes)
            {
                load_people(option.position);
            }
            groups->addSpacing(8);
        }
        int position = scroll->verticalScrollBar()->value();
        scroll->setWidget(body);
        scroll->verticalScrollBar()->setValue(position);
    };
    QObject::connect(&client,
                     &client_bridge::poll_received,
                     &dialog,
                     [&](quint64 request, qint64 r, qint64 id, chat::poll_state state, QString error)
                     {
                         if (r != room || id != message || request != query)
                         {
                             return;
                         }
                         query = 0;
                         if (!error.isEmpty())
                         {
                             status->setText(error);
                             return;
                         }
                         question->setText(QString::fromStdString(state.question));
                         render(state.summary);
                     });
    QObject::connect(&client,
                     &client_bridge::poll_changed,
                     &dialog,
                     [&](chat::poll_update update)
                     {
                         if (update.conversation == room && update.message == message)
                         {
                             render(update.summary);
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::voters_received,
                     &dialog,
                     [&](quint64 req, qint64 id, qint64 choice, qint64 after, chat::poll_voters page, QString error)
                     {
                         auto it = sections.find(choice);
                         if (id != message || it == sections.end() || req != it->request || after != it->after)
                         {
                             return;
                         }
                         it->request = 0;
                         it->more->setEnabled(true);
                         if (!error.isEmpty())
                         {
                             it->more->setText(QStringLiteral("重试加载成员"));
                             it->more->show();
                             return;
                         }
                         for (auto const& member : page.members)
                         {
                             auto name = QString::fromStdString(member.nickname.empty() ? member.username : member.nickname);
                             auto* row = new QHBoxLayout;
                             auto* avatar = new QLabel;
                             chat.avatars().observe(member.id, member.avatar);
                             auto refresh = [&, avatar, user = member.id, name]
                             { avatar->setPixmap(avatar_icon(name, 26, chat.avatars().image(user)).pixmap(26, 26)); };
                             refresh();
                             QObject::connect(&chat.avatars(),
                                              &avatar_cache::changed,
                                              avatar,
                                              [refresh, user = member.id](qint64 id)
                                              {
                                                  if (id == user)
                                                  {
                                                      refresh();
                                                  }
                                              });
                             row->addWidget(avatar);
                             auto* label = new QLabel(name);
                             label->setTextFormat(Qt::PlainText);
                             label->setObjectName(QStringLiteral("pollVoter"));
                             label->setProperty("choice", choice);
                             label->setToolTip(QString::fromStdString(member.username));
                             row->addWidget(label, 1);
                             it->people->addLayout(row);
                         }
                         it->after = page.next;
                         it->more->setText(QStringLiteral("更多投票成员"));
                         it->more->setVisible(page.next > 0);
                     });
    QObject::connect(&chat,
                     &chat_widget::history_discarded,
                     &dialog,
                     [&](qint64 id, qint64 through)
                     {
                         if (id == room && message <= through)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::conversation_changed,
                     &dialog,
                     [&](qint64 id, bool removed)
                     {
                         if (id == room)
                         {
                             if (removed)
                             {
                                 dialog.reject();
                             }
                             else
                             {
                                 reload();
                             }
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::message_updated,
                     &dialog,
                     [&](qint64 id, message_data m, QString)
                     {
                         if (id == room && m.id == message && m.deleted)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client, &client_bridge::disconnected, &dialog, &QDialog::reject);
    reload();
    dialog.exec();
}
