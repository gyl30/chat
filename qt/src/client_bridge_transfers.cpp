#include "client_bridge.hpp"
#include "clipboard_image.hpp"

#include <optional>
#include <utility>

#include <QFile>
#include <QMetaObject>
#include <QTemporaryDir>

void client_bridge::change_file(qint64 conversation, QString key, bool add)
{
    auto handler = [this, conversation, g = connection_generation_.load()](auto result)
    {
        post_result(g,
                    [this, conversation, result = std::move(result)]
                    { emit files_changed(conversation, result ? QString{} : display_error(result.error())); });
    };
    if (add)
    {
        client_->add_conversation_file(conversation, key.toStdString(), std::move(handler));
    }
    else
    {
        client_->remove_conversation_file(conversation, key.toStdString(), std::move(handler));
    }
}
quint64 client_bridge::list_files(qint64 conversation, QString search, QString category, qint64 before, QString key)
{
    auto const request = ++files_request_;
    client_->list_conversation_files(conversation,
                                     search.toStdString(),
                                     category.toStdString(),
                                     before,
                                     key.toStdString(),
                                     [this, request, before, g = connection_generation_.load()](auto r)
                                     {
                                         post_result(g,
                                                     [this, request, before, r = std::move(r)]() mutable
                                                     {
                                                         emit files_received(request,
                                                                             r ? std::move(*r) : chat::conversation_file_page{},
                                                                             before != 0,
                                                                             r ? QString{} : display_error(r.error()));
                                                     });
                                     });
    return request;
}
void client_bridge::download_file(qint64 conversation, QString key, QString filename, QString path, bool open)
{
    emit download_requested(conversation, 0, std::move(key), std::move(filename), std::move(path), open);
}
void client_bridge::send_attachment(qint64 conversation, QString path, qint64 reply_to, qint64 client_id, bool group_only)
{
    auto progress = [this, client_id, g = connection_generation_.load()](auto bytes, auto total)
    { post_result(g, [this, client_id, bytes, total] { emit attachment_progress(client_id, bytes, total); }); };
    if (group_only)
    {
        client_->upload_conversation_file(
            conversation,
            path.toStdString(),
            client_id,
            [this, conversation, client_id, g = connection_generation_.load()](auto r)
            {
                post_result(g,
                            [this, conversation, client_id, r = std::move(r)]
                            {
                                auto error = r ? QString{} : display_error(r.error());
                                auto const unavailable =
                                    !r && r.error().kind == chat::error_kind::rpc && r.error().code == chat::conversation_unavailable;
                                emit group_file_finished(client_id, error, unavailable);
                                emit files_changed(conversation, error);
                            });
            },
            std::move(progress));
    }
    else
    {
        client_->send_attachment(conversation,
                                 path.toStdString(),
                                 send_result(conversation, client_id, path),
                                 reply_to > 0 ? std::optional<std::int64_t>{reply_to} : std::nullopt,
                                 client_id,
                                 std::move(progress));
    }
}
void client_bridge::restore_attachments()
{
    client_->pending_attachments(
        [this, g = connection_generation_.load()](auto result)
        {
            post_result(g,
                        [this, result = std::move(result)]
                        {
                            if (!result)
                            {
                                emit error(display_error(result.error()));
                                return;
                            }
                            for (auto const& t : *result)
                            {
                                emit attachment_restored(
                                    t.conversation, t.client_id, QString::fromStdString(t.path), t.size, t.reply_to, t.confirmed, t.group_file);
                            }
                        });
        });
}
void client_bridge::interrupt_attachment(qint64 id, bool cancel, QString path)
{
    client_->interrupt_attachment(id,
                                  cancel,
                                  [this, id, cancel, path = std::move(path), g = connection_generation_.load()](auto result)
                                  {
                                      if (result && cancel)
                                      {
                                          remove_clipboard_image(path, id);
                                      }
                                      post_result(g,
                                                  [this, id, cancel, result = std::move(result)]
                                                  {
                                                      if (!result)
                                                      {
                                                          emit error(display_error(result.error()));
                                                          return;
                                                      }
                                                      if (cancel)
                                                      {
                                                          emit attachment_cancelled(id);
                                                      }
                                                  });
                                  });
}

std::function<void(std::expected<chat::message, chat::error>)> client_bridge::send_result(qint64 conversation, qint64 client_id, QString path)
{
    // Release the acknowledged source even if the GUI has already changed sessions.
    return [this, conversation, client_id, path = std::move(path), generation = connection_generation_.load()](auto result)
    {
        if (result)
        {
            remove_clipboard_image(path, client_id);
        }
        post_result(generation,
                    [this, conversation, client_id, result = std::move(result)]
                    {
                        auto status = send_status::sent;
                        if (!result)
                        {
                            auto const& error = result.error();
                            status = error.kind == chat::error_kind::rpc && error.code == chat::conversation_unavailable
                                         ? send_status::unavailable
                                         : send_status::failed;
                        }
                        emit send_finished(conversation,
                                           client_id,
                                           result ? to_message_data(*result) : message_data{},
                                           result ? QString{} : display_error(result.error()),
                                           status);
                    });
    };
}

void client_bridge::get_attachment(qint64 conversation, qint64 message, QString filename, QString target, qint64 item)
{
    emit download_requested(conversation, message, {}, std::move(filename), std::move(target), false, item);
}

void client_bridge::start_download(qint64 id,
                                   quint64 attempt,
                                   qint64 conversation,
                                   qint64 message,
                                   QString key,
                                   QString path,
                                   std::shared_ptr<chat::download_control> control,
                                   bool replace,
                                   qint64 item)
{
    // The task list survives reconnects; a completion from the failed connection still retires this attempt.
    auto progress = [this, id, attempt](qint64 bytes, qint64 total)
    {
        QMetaObject::invokeMethod(
            this, [this, id, attempt, bytes, total] { emit download_progress(id, attempt, bytes, total); }, Qt::QueuedConnection);
    };
    auto done = [this, id, attempt](auto result)
    {
        auto error = result ? QString{} : display_error(result.error());
        QMetaObject::invokeMethod(
            this, [this, id, attempt, error = std::move(error)] { emit download_finished(id, attempt, error); }, Qt::QueuedConnection);
    };
    chat::download_options options{std::move(control), std::move(progress), replace};
    if (message)
    {
        client_->get_attachment(conversation, message, path.toStdString(), std::move(done), chat::max_attachment_size, std::move(options), item);
    }
    else
    {
        client_->download_conversation_file(conversation, key.toStdString(), path.toStdString(), std::move(done), std::move(options));
    }
}


quint64 client_bridge::get_message_image(qint64 conversation, qint64 message, qint64 item)
{
    auto const request = item ? ++content_request_ : 0;
    auto tmp = std::make_shared<QTemporaryDir>();
    if (!tmp->isValid())
    {
        if (item)
        {
            post_result(connection_generation_.load(),
                        [this, request, message, item, error = tmp->errorString()]
                        { emit bundle_image_received(request, message, item, {}, error); });
        }
        else
        {
            emit message_image_received(conversation, message, {}, tmp->errorString());
        }
        return request;
    }
    auto path = tmp->filePath("preview");
    client_->get_attachment(
        conversation,
        message,
        path.toStdString(),
        [this, request, tmp, path, conversation, message, item, g = connection_generation_.load()](auto result)
        {
            QFile file(path);
            auto error = result ? QString{} : display_error(result.error());
            QByteArray bytes;
            if (result)
            {
                if (file.open(QIODevice::ReadOnly))
                {
                    bytes = file.read(chat::max_image_preview_size);
                }
                if (!file.isOpen() || file.error() != QFileDevice::NoError)
                {
                    error = file.errorString();
                }
            }
            post_result(g,
                        [this, request, conversation, message, item, bytes = std::move(bytes), error = std::move(error)]
                        {
                            if (item)
                            {
                                emit bundle_image_received(request, message, item, bytes, error);
                            }
                            else
                            {
                                emit message_image_received(conversation, message, bytes, error);
                            }
                        });
        },
        chat::max_image_preview_size,
        {},
        item);
    return request;
}
