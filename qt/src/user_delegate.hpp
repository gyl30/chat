#ifndef CHAT_QT_SRC_USER_DELEGATE_HPP
#define CHAT_QT_SRC_USER_DELEGATE_HPP

#include <QStyledItemDelegate>
#include <functional>

class user_delegate : public QStyledItemDelegate
{
    Q_OBJECT
   public:
    explicit user_delegate(QObject* parent = nullptr, std::function<QString(QModelIndex const&)> status = {});
    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index) override;

   private:
    std::function<QString(QModelIndex const&)> status_;

   signals:
    void avatar_clicked(QModelIndex index);
    void body_clicked(QModelIndex index);
};

#endif
