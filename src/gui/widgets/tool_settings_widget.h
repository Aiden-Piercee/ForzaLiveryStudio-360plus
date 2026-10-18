#pragma once

#include <QHash>
#include <QVariant>
#include <QWidget>
#include <functional>

class QHBoxLayout;

namespace gui {

class ToolSettingsWidget final : public QWidget {
public:
    explicit ToolSettingsWidget(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void setTool(const QString &tool);
    void setValue(const QString &key, const QVariant &value);
    std::function<void(const QString &, const QVariant &)> optionChanged;
    std::function<void()> editingFinished;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void finishEditing();
    void addCheck(const QString &key, const QString &text, const QStringList &tools);
    void addChoice(const QString &key, const QString &text, const QStringList &choices, const QStringList &tools);
    void addNumber(const QString &key, const QString &text, double minimum, double maximum,
                   int decimals, double step, const QStringList &tools, const QString &suffix = {});
    void registerField(const QString &key, QWidget *field, QWidget *container, const QStringList &tools);
    QHBoxLayout *layout_ = nullptr;
    QHash<QString, QWidget *> fields_;
    QHash<QWidget *, QStringList> tools_;
};

} // namespace gui
