#include "poll_dialog.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include <QDialogButtonBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSet>

std::optional<std::pair<QString, QStringList>> compose_poll(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("发起群投票"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* question = new QLineEdit(&dialog);
    question->setPlaceholderText(QStringLiteral("投票问题（最多 256 字）"));
    auto* options = new QPlainTextEdit(&dialog);
    options->setPlaceholderText(QStringLiteral("每行一个选项，2 至 10 项，每项最多 80 字"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(question);
    layout->addWidget(options);
    layout->addWidget(buttons);
    auto values = [&]
    {
        auto labels = options->toPlainText().trimmed().split(u'\n');
        for (auto& label : labels)
        {
            label = label.trimmed();
        }
        return labels;
    };
    auto validate = [&]
    {
        auto q = question->text().trimmed();
        auto labels = values();
        QSet<QString> seen;
        bool valid = !q.isEmpty() && q.toUcs4().size() <= 256 && labels.size() >= 2 && labels.size() <= 10;
        for (auto const& label : labels)
        {
            valid = valid && !label.isEmpty() && label.toUcs4().size() <= 80 && !seen.contains(label);
            seen.insert(label);
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    QObject::connect(question, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(options, &QPlainTextEdit::textChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return std::pair{question->text().trimmed(), values()};
}

void show_poll(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("群投票"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* question = new QLabel(&dialog);
    question->setWordWrap(true);
    question->setTextFormat(Qt::PlainText);
    auto* options = new QListWidget(&dialog);
    auto* status = new QLabel(&dialog);
    status->setTextFormat(Qt::PlainText);
    auto* actions = new QHBoxLayout;
    auto* submit = new QPushButton(QStringLiteral("提交投票"), &dialog);
    auto* cancel = new QPushButton(QStringLiteral("取消我的投票"), &dialog);
    auto* close = new QPushButton(QStringLiteral("结束投票"), &dialog);
    auto* people = new QListWidget(&dialog);
    auto* more = new QPushButton(QStringLiteral("更多投票成员"), &dialog);
    layout->addWidget(question);
    layout->addWidget(options);
    layout->addWidget(status);
    layout->addLayout(actions);
    actions->addWidget(submit);
    actions->addWidget(cancel);
    actions->addWidget(close);
    layout->addWidget(new QLabel(QStringLiteral("当前选项的投票成员"), &dialog));
    layout->addWidget(people);
    layout->addWidget(more);
    qint64 cursor = 0;
    quint64 query = 0, operation = 0, voters_query = 0;
    auto reload = [&]
    {
        voters_query = 0;
        more->setEnabled(false);
        query = client.get_poll(room, message);
    };
    chat::poll_state state;
    auto controls = [&]
    {
        submit->setEnabled(!operation && !state.summary.closed && options->currentRow() >= 0 && options->currentRow() + 1 != state.selection);
        cancel->setEnabled(!operation && !state.summary.closed && state.selection != 0);
        close->setEnabled(!operation);
        close->setVisible(state.can_close && !state.summary.closed);
    };
    auto load_people = [&]
    {
        voters_query = 0;
        cursor = 0;
        more->hide();
        people->clear();
        controls();
        if (options->currentRow() >= 0)
        {
            voters_query = client.get_poll_voters(room, message, options->currentRow() + 1, 0);
        }
    };
    QObject::connect(options, &QListWidget::currentRowChanged, &dialog, load_people);
    QObject::connect(&client,
                     &client_bridge::poll_received,
                     &dialog,
                     [&](quint64 request, qint64 r, qint64 id, chat::poll_state value, QString err)
                     {
                         if (r != room || id != message)
                         {
                             return;
                         }
                         if (request == operation)
                         {
                             operation = 0;
                             controls();
                             if (!err.isEmpty())
                             {
                                 status->setText(err);
                             }
                             // Successful mutations also publish poll_changed. That refresh reads
                             // the current selection and permissions, even after a dialog was reopened.
                             return;
                         }
                         if (request != query)
                         {
                             return;
                         }
                         query = 0;
                         if (!err.isEmpty())
                         {
                             status->setText(err);
                             submit->setEnabled(false);
                             cancel->setEnabled(false);
                             close->hide();
                             return;
                         }
                         if (value.summary.revision < state.summary.revision)
                         {
                             return;
                         }
                         auto prev = options->currentRow();
                         state = std::move(value);
                         question->setText(QString::fromStdString(state.question));
                         qint64 total = 0;
                         for (auto const& option : state.summary.options)
                         {
                             total += option.votes;
                         }
                         {
                             QSignalBlocker blocker(options);
                             options->clear();
                             for (auto const& option : state.summary.options)
                             {
                                 options->addItem(QStringLiteral("%1 %2　%3 票（%4%）")
                                                      .arg(option.position == state.selection ? QStringLiteral("●") : QStringLiteral("○"),
                                                           QString::fromStdString(option.label))
                                                      .arg(option.votes)
                                                      .arg(total ? 100. * option.votes / total : 0., 0, 'f', 1));
                             }
                             options->setCurrentRow(prev >= 0 ? prev : state.selection ? int(state.selection) - 1 : 0);
                         }
                         status->setText(state.summary.closed ? QStringLiteral("投票已结束") : QStringLiteral("实名单选 · 共 %1 票").arg(total));
                         load_people();
                     });
    QObject::connect(&client,
                     &client_bridge::poll_changed,
                     &dialog,
                     [&](chat::poll_update const& update)
                     {
                         if (update.message == message && update.summary.revision > state.summary.revision)
                         {
                             reload();
                         }
                     });
    QObject::connect(
        &client,
        &client_bridge::voters_received,
        &dialog,
        [&](quint64 request, qint64 id, qint64 choice, qint64 after, chat::poll_voters page, QString err)
        {
            if (request != voters_query || id != message || choice != options->currentRow() + 1 || after != cursor)
            {
                return;
            }
            if (!err.isEmpty())
            {
                status->setText(err);
                return;
            }
            if (!after)
            {
                people->clear();
            }
            for (auto const& member : page.members)
            {
                auto name = QString::fromStdString(member.username);
                people->addItem(member.nickname.empty() ? name : QStringLiteral("%1 (@%2)").arg(QString::fromStdString(member.nickname), name));
            }
            cursor = page.next;
            more->setVisible(cursor > 0);
            more->setEnabled(true);
        });
    QObject::connect(more,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         more->setEnabled(false);
                         voters_query = client.get_poll_voters(room, message, options->currentRow() + 1, cursor);
                     });
    QObject::connect(submit,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         operation = client.vote_poll(room, message, options->currentRow() + 1);
                         controls();
                     });
    QObject::connect(cancel,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         operation = client.vote_poll(room, message, 0);
                         controls();
                     });
    QObject::connect(close,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         operation = client.close_poll(room, message);
                         controls();
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
    controls();
    more->hide();
    reload();
    dialog.exec();
}
