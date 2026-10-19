// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui-bridge.h"
#include <obs-module.h>
#include "track-defaults.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <array>

namespace {
// Text from this module's data/locale files, as OBS loads them.
QString T(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

QPointer<QDialog> settingsDialog;
QPointer<QTimer> assignmentTimer;

QJsonObject snapshot()
{
	char *json = ea_ui_snapshot();
	auto data = QJsonDocument::fromJson(QByteArray(json)).object(); ea_ui_free(json); return data;
}

void showError(QWidget *parent, const char *error)
{
	QMessageBox::warning(parent, T("Panel.Title"), QString::fromUtf8(error));
}

void refreshAssignments()
{
	QList<QWidget *> dialogs;
	for (auto *widget : QApplication::topLevelWidgets())
		if (widget->inherits("OBSBasicAdvAudio") && widget->isVisible()) dialogs.append(widget);
	if (dialogs.isEmpty()) return;
	const auto data = snapshot(); const bool busy = data["busy"].toBool();
	const auto tracks = data["tracks"].toArray(), sources = data["sources"].toArray();
	for (auto *dialog : dialogs) {
		auto *grid = dialog->findChild<QGridLayout *>(QStringLiteral("mainLayout"));
		if (!grid) continue;
		for (int row = 1; row < grid->rowCount(); ++row) {
			auto *nameItem = grid->itemAtPosition(row, 1);
			auto *name = nameItem ? qobject_cast<QLabel *>(nameItem->widget()) : nullptr;
			if (!name) continue;
			const QString fullName = name->toolTip().isEmpty() ? name->text() : name->toolTip();
			QJsonObject source;
			for (auto value : sources) if (value.toObject()["name"].toString() == fullName) { source = value.toObject(); break; }
			if (source.isEmpty()) continue; // Never infer a row from a truncated or stale name.
			QWidget *container = nullptr;
			for (int col = 7; col < grid->columnCount(); ++col) {
				auto *item = grid->itemAtPosition(row, col); auto *candidate = item ? item->widget() : nullptr;
				if (candidate && qobject_cast<QBoxLayout *>(candidate->layout()) && candidate->findChildren<QCheckBox *>().size() >= 6) { container = candidate; break; }
			}
			if (!container) continue;
			auto *layout = qobject_cast<QBoxLayout *>(container->layout());
			for (auto value : tracks) {
				const auto track = value.toObject(); const int number = track["number"].toInt();
				const bool enabled = track["enabled"].toBool();
				const auto id = QStringLiteral("ExtraAudioTrack%1").arg(number);
				auto *box = container->findChild<QCheckBox *>(id, Qt::FindDirectChildrenOnly);
				if (!enabled) { if (box) delete box; continue; }
				if (!box) {
					box = new QCheckBox(QString::number(number), container); box->setObjectName(id);
					box->setAccessibleName(T("Assign.Accessible").arg(fullName).arg(number));
					// Insert before any higher-numbered extra, including when a lower slot is re-enabled.
					int position = layout->count();
					for (int i = 0; i < layout->count(); ++i) {
						auto *other = layout->itemAt(i)->widget();
						if (other && other->property("extraAudioNumber").toInt() > number) { position = i; break; }
					}
					box->setProperty("extraAudioNumber", number); layout->insertWidget(position, box);
					QObject::connect(box, &QCheckBox::clicked, box, [box, dialog, number](bool checked) {
						const auto uuid = box->property("sourceUuid").toString().toUtf8(); char error[1024] = {};
						if (!ea_ui_assign(uuid.constData(), unsigned(number), checked, error, sizeof(error))) {
							QSignalBlocker blocker(box); box->setChecked(!checked); showError(dialog, error);
						}
					});
				}
				box->setProperty("sourceUuid", source["uuid"].toString());
				const bool independent = source["independent"].toBool();
				box->setEnabled(independent && !busy);
				{ QSignalBlocker blocker(box); box->setChecked((source["routes"].toInt() & (1 << (number - 7))) != 0); }
				QString hint = T("Assign.Hint").arg(number).arg(track["name"].toString()).arg(track["bitrate"].toInt());
				if (!independent) hint += QStringLiteral("\n") + T("Assign.NotIndependent");
				if (busy) hint += QStringLiteral("\n") + T("Assign.Busy");
				box->setToolTip(hint);
			}
		}
	}
}

struct TrackControls {
	QCheckBox *enabled, *main, *vertical;
	QLineEdit *name;
	QComboBox *bitrate;
};

bool advAudioVisible()
{
	for (auto *widget : QApplication::topLevelWidgets())
		if (widget->inherits("OBSBasicAdvAudio") && widget->isVisible()) return true;
	return false;
}

// Refresh the extra-track checkboxes only while Advanced Audio Properties is
// open. OBS creates that dialog as a child of its main window when first opened.
class AdvAudioWatcher : public QObject {
public:
	using QObject::QObject;
	void watch(QObject *dialog)
	{
		if (dialog->property("extraAudioWatched").toBool()) return;
		dialog->setProperty("extraAudioWatched", true); dialog->installEventFilter(this);
		if (static_cast<QWidget *>(dialog)->isVisible()) startRefresh();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::ChildPolished) {
			QObject *child = static_cast<QChildEvent *>(event)->child();
			if (child && child->inherits("OBSBasicAdvAudio")) watch(child);
		} else if (event->type() == QEvent::Show && watched->inherits("OBSBasicAdvAudio")) {
			startRefresh();
		} else if (event->type() == QEvent::Hide && watched->inherits("OBSBasicAdvAudio")) {
			QTimer::singleShot(0, this, [] { if (assignmentTimer && !advAudioVisible()) assignmentTimer->stop(); });
		}
		return false;
	}

private:
	void startRefresh()
	{
		if (!assignmentTimer) return;
		QTimer::singleShot(0, assignmentTimer, refreshAssignments);
		assignmentTimer->start();
	}
};
QPointer<AdvAudioWatcher> advAudioWatcher;
}

extern "C" void ea_qt_init(void *parent)
{
	if (assignmentTimer || !qApp || !parent) return;
	auto *window = static_cast<QWidget *>(parent);
	assignmentTimer = new QTimer(window); assignmentTimer->setInterval(350);
	QObject::connect(assignmentTimer, &QTimer::timeout, assignmentTimer, refreshAssignments);
	advAudioWatcher = new AdvAudioWatcher(window); window->installEventFilter(advAudioWatcher);
	for (auto *widget : QApplication::topLevelWidgets()) if (widget->inherits("OBSBasicAdvAudio")) advAudioWatcher->watch(widget);
}

extern "C" void ea_qt_close(void)
{
	if (settingsDialog) delete settingsDialog.data();
}

extern "C" void ea_qt_shutdown(void)
{
	ea_qt_close(); if (assignmentTimer) delete assignmentTimer.data();
	if (advAudioWatcher) delete advAudioWatcher.data();
	for (auto *widget : QApplication::topLevelWidgets()) if (widget->inherits("OBSBasicAdvAudio")) {
		const auto boxes = widget->findChildren<QCheckBox *>();
		for (auto *box : boxes) if (box->objectName().startsWith(QStringLiteral("ExtraAudioTrack"))) delete box;
	}
}

extern "C" void ea_qt_defer(void (*task)(void *), void *param)
{
	// obs_queue_task(OBS_TASK_UI) runs immediately when called on the UI thread,
	// where OBS invokes hotkey callbacks while holding its hotkey lock.
	if (qApp) QMetaObject::invokeMethod(qApp, [task, param] { task(param); }, Qt::QueuedConnection);
}

extern "C" void ea_qt_open(void *)
{
	if (settingsDialog) { settingsDialog->show(); settingsDialog->raise(); settingsDialog->activateWindow(); return; }
	auto *parent = static_cast<QWidget *>(ea_ui_parent()); ea_qt_init(parent);
	auto initial = snapshot(); auto *dialog = new QDialog(parent); settingsDialog = dialog;
	dialog->setObjectName(QStringLiteral("ExtraAudioTracksDialog")); dialog->setWindowTitle(T("Panel.Title"));
	// 680 px plus the title bar fits a 768 px screen above the taskbar.
	dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->resize(800, 680);
	auto *layout = new QVBoxLayout(dialog);
	// Enabled tracks apply to both tabs: what Tracks edits and what Backup saves.
	auto *enableGroup = new QGroupBox(T("Panel.ExtraTracks"), dialog); auto *enableRow = new QHBoxLayout(enableGroup); layout->addWidget(enableGroup);
	enableGroup->setToolTip(T("Panel.ExtraTracks.Tooltip"));
	// Tracks and backups each get the full height; Status stays visible below both.
	auto *tabs = new QTabWidget(dialog); tabs->setObjectName(QStringLiteral("EATabs")); layout->addWidget(tabs, 1);
	auto *tracksTab = new QWidget(tabs); tracksTab->setObjectName(QStringLiteral("EATracksTab"));
	auto *tracksLayout = new QVBoxLayout(tracksTab); tabs->addTab(tracksTab, T("Panel.TracksTab"));
	tabs->setTabToolTip(0, T("Panel.TracksTab.Tooltip"));
	auto *roles = new QLabel(T("Panel.Roles")
		.arg(initial["live"].toInt(1)).arg(initial["vod"].toInt(2)), tracksTab); tracksLayout->addWidget(roles);
	roles->setObjectName(QStringLiteral("EAOutputRoles"));
	roles->setToolTip(T("Panel.Roles.Tooltip"));
	auto *encoderLabel = new QLabel(T("Panel.Encoder").arg(initial["encoder"].toString()), tracksTab);
	encoderLabel->setObjectName(QStringLiteral("EARecordingEncoder")); tracksLayout->addWidget(encoderLabel);
	encoderLabel->setToolTip(T("Panel.Encoder.Tooltip"));
	auto *scroll = new QScrollArea(tracksTab); scroll->setObjectName(QStringLiteral("ExtraTrackScrollArea"));
	scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->viewport()->setAutoFillBackground(false);
	auto *trackList = new QWidget(scroll); trackList->setAutoFillBackground(false);
	auto *trackLayout = new QVBoxLayout(trackList); trackLayout->setContentsMargins(0, 0, 0, 0); trackLayout->setSpacing(6);
	scroll->setWidget(trackList); tracksLayout->addWidget(scroll, 1);
	auto *empty = new QLabel(T("Panel.NoTracks"), tracksTab); tracksLayout->addWidget(empty);
	std::array<TrackControls, 10> controls{}; int enabledCount = 0;
	const auto trackValues = initial["tracks"].toArray();
	for (int i = 0; i < 10; ++i) {
		const int number = i + 7; const auto track = trackValues[i].toObject(); auto &c = controls[i];
		c.enabled = new QCheckBox(QString::number(number), enableGroup); c.enabled->setObjectName(QStringLiteral("EAEnable%1").arg(number));
		c.enabled->setAccessibleName(T("Track.EnableAccessible").arg(number)); c.enabled->setChecked(track["enabled"].toBool()); enableRow->addWidget(c.enabled);
		auto *page = new QGroupBox(T("Track.Title").arg(number), trackList);
		page->setObjectName(QStringLiteral("EATrackGroup%1").arg(number));
		auto *form = new QFormLayout(page); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter); form->setContentsMargins(9, 2, 9, 9);
		c.name = new QLineEdit(track["name"].toString(), page); c.name->setObjectName(QStringLiteral("EAName%1").arg(number));
		c.name->setAccessibleName(T("Track.NameAccessible").arg(number));
		c.bitrate = new QComboBox(page); c.bitrate->setObjectName(QStringLiteral("EABitrate%1").arg(number));
		c.bitrate->setAccessibleName(T("Track.BitrateAccessible").arg(number));
		// The bitrates the recording audio encoder itself offers in OBS Settings.
		for (const auto option : initial["bitrates"].toArray()) {
			const int kbps = option.toObject()["kbps"].toInt(); c.bitrate->addItem(QString::number(kbps), kbps);
		}
		int rate = track["bitrate"].toInt(EA_DEFAULT_RECORD_BITRATE), index = c.bitrate->findData(rate);
		if (index < 0) {
			c.bitrate->addItem(c.bitrate->count() ? T("Track.BitrateNotOffered").arg(rate) : QString::number(rate), rate);
			index = c.bitrate->count() - 1;
		}
		c.bitrate->setCurrentIndex(index); c.bitrate->setToolTip(T("Track.Bitrate.Tooltip"));
		auto *bitrateLabel = new QLabel(T("Track.Bitrate"), page); bitrateLabel->setMinimumWidth(170);
		bitrateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		bitrateLabel->setBuddy(c.bitrate); bitrateLabel->setToolTip(T("Track.BitrateLabel.Tooltip"));
		form->addRow(bitrateLabel, c.bitrate); form->addRow(T("Track.Name"), c.name);
		c.main = new QCheckBox(T("Track.Main"), page);
		c.vertical = new QCheckBox(T("Track.Vertical"), page);
		c.main->setToolTip(T("Track.Main.Tooltip"));
		c.vertical->setToolTip(T("Track.Vertical.Tooltip"));
		c.main->setAccessibleName(T("Track.MainAccessible").arg(number));
		c.vertical->setAccessibleName(T("Track.VerticalAccessible").arg(number));
		c.main->setChecked(track["main"].toBool()); c.vertical->setChecked(track["vertical"].toBool());
		auto *recordings = new QHBoxLayout(); recordings->setContentsMargins(0, 0, 0, 0);
		recordings->addWidget(c.main, 1); recordings->addWidget(c.vertical, 1); form->addRow(T("Track.EmbedIn"), recordings);
		trackLayout->addWidget(page); page->setVisible(c.enabled->isChecked());
		if (c.enabled->isChecked()) ++enabledCount;
		QObject::connect(c.enabled, &QCheckBox::toggled, dialog, [page, trackList, scroll, empty](bool enabled) {
			page->setVisible(enabled);
			bool any = false;
			for (auto *group : trackList->findChildren<QGroupBox *>(QString(), Qt::FindDirectChildrenOnly)) any |= !group->isHidden();
			empty->setVisible(!any);
			if (enabled) QTimer::singleShot(0, page, [scroll, page] { scroll->ensureWidgetVisible(page); });
		});
	}
	trackLayout->addStretch();
	empty->setVisible(enabledCount == 0);
	auto *backupTab = new QWidget(tabs); backupTab->setObjectName(QStringLiteral("EABackupTab"));
	auto *backupTabLayout = new QVBoxLayout(backupTab); tabs->addTab(backupTab, T("Panel.BackupTab"));
	tabs->setTabToolTip(1, T("Panel.BackupTab.Tooltip"));
	auto *backupGroup = new QGroupBox(T("Backup.Group"), backupTab); backupTabLayout->addWidget(backupGroup);
	auto *backupLayout = new QVBoxLayout(backupGroup);
	auto *backupEnabled = new QCheckBox(T("Backup.Enable"), backupGroup);
	backupEnabled->setObjectName(QStringLiteral("EABackupMP3")); backupEnabled->setChecked(initial["backup_mp3"].toBool());
	backupEnabled->setToolTip(T("Backup.Enable.Tooltip"));
	backupLayout->addWidget(backupEnabled);
	auto *backupFormat = new QLineEdit(initial["backup_format"].toString(), backupGroup);
	backupFormat->setObjectName(QStringLiteral("EABackupFilenameFormat")); backupFormat->setAccessibleName(T("Backup.FormatAccessible"));
	backupFormat->setPlaceholderText(QStringLiteral(EA_DEFAULT_BACKUP_FORMAT));
	backupFormat->setToolTip(T("Backup.Format.Tooltip").arg(QStringLiteral(EA_DEFAULT_BACKUP_FORMAT)));
	auto *backupForm = new QFormLayout(); backupForm->addRow(T("Backup.Format"), backupFormat); backupLayout->addLayout(backupForm);
	backupFormat->setEnabled(backupEnabled->isChecked()); QObject::connect(backupEnabled, &QCheckBox::toggled, backupFormat, &QLineEdit::setEnabled);
	// A live example from the same code that names the real backups, and how many files each recording gets.
	auto *backupExample = new QLabel(backupGroup); backupExample->setObjectName(QStringLiteral("EABackupExample"));
	backupExample->setWordWrap(true); backupExample->setTextInteractionFlags(Qt::TextSelectableByMouse); backupLayout->addWidget(backupExample);
	backupExample->setEnabled(backupEnabled->isChecked()); QObject::connect(backupEnabled, &QCheckBox::toggled, backupExample, &QLabel::setEnabled);
	const bool verticalSupported = initial["vertical_supported"].toBool();
	auto refreshBackup = [controls, backupFormat, backupExample, verticalSupported] {
		int enabled = 0, first = -1;
		for (int i = 0; i < 10; ++i) if (controls[i].enabled->isChecked()) { if (first < 0) first = i; ++enabled; }
		if (first < 0) { backupExample->setText(T("Backup.NoTracks")); return; }
		char example[2048] = {};
		const QByteArray pattern = backupFormat->text().toUtf8(), name = controls[first].name->text().toUtf8();
		const QString line = ea_ui_backup_example(pattern.constData(), unsigned(first + 7), name.constData(), example, sizeof(example))
			? T("Backup.Example").arg(QString::fromUtf8(example)) : T("Backup.ExampleInvalid");
		const QString sets = verticalSupported ? T("Backup.SetsVertical").arg(enabled).arg(enabled * 2) : T("Backup.SetsMain").arg(enabled);
		backupExample->setText(line + QStringLiteral("\n") + sets);
	};
	QObject::connect(backupFormat, &QLineEdit::textChanged, dialog, refreshBackup);
	for (const auto &c : controls) {
		QObject::connect(c.enabled, &QCheckBox::toggled, dialog, refreshBackup);
		QObject::connect(c.name, &QLineEdit::textChanged, dialog, refreshBackup);
	}
	refreshBackup();
	backupTabLayout->addStretch();
	// Missing sources, an unsupported Aitum version and finalizing results would
	// otherwise appear only in the OBS log. Long job lists scroll inside the box.
	auto *statusGroup = new QGroupBox(T("Panel.Status"), dialog); statusGroup->setObjectName(QStringLiteral("EAStatusGroup"));
	auto *statusLayout = new QVBoxLayout(statusGroup);
	auto *statusScroll = new QScrollArea(statusGroup); statusScroll->setFrameShape(QFrame::NoFrame);
	statusScroll->setWidgetResizable(true); statusScroll->viewport()->setAutoFillBackground(false);
	auto *status = new QLabel(statusScroll); status->setObjectName(QStringLiteral("EAStatus")); status->setAccessibleName(T("Panel.StatusAccessible"));
	status->setWordWrap(true); status->setAlignment(Qt::AlignLeft | Qt::AlignTop); status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	statusScroll->setWidget(status); statusLayout->addWidget(statusScroll); layout->addWidget(statusGroup);
	auto refreshStatus = [status, statusScroll] {
		char text[8192] = {}; bool busy = false; ea_ui_status(text, sizeof(text), &busy);
		QString value = QString::fromUtf8(text).trimmed();
		if (busy) value = T("Panel.StopOutputs") + (value.isEmpty() ? QString() : QStringLiteral("\n") + value);
		if (status->text() != value) status->setText(value);
		// Fit the wrapped text, one to five lines, so the track list keeps the rest.
		const int line = status->fontMetrics().lineSpacing();
		const int wanted = status->heightForWidth(qMax(1, statusScroll->viewport()->width()));
		const int height = qBound(line, wanted, line * 5) + 4;
		if (statusScroll->height() != height) statusScroll->setFixedHeight(height);
	};
	// Only read applied OBS settings. Refresh labels without reloading the track draft
	// or changing checkbox states, names, bitrates, or source assignments.
	auto refreshRoles = [roles, encoderLabel] {
		struct ea_ui_roles selected{}; if (!ea_ui_read_roles(&selected)) return;
		const auto summary = T("Panel.Roles")
			.arg(selected.live).arg(selected.vod);
		if (roles->text() != summary) roles->setText(summary);
		const auto encoder = T("Panel.Encoder").arg(QString::fromUtf8(selected.encoder));
		if (encoderLabel->text() != encoder) encoderLabel->setText(encoder);
	};
	auto *roleTimer = new QTimer(dialog); roleTimer->setInterval(350);
	QObject::connect(roleTimer, &QTimer::timeout, dialog, refreshRoles); refreshRoles();
	QObject::connect(roleTimer, &QTimer::timeout, dialog, refreshStatus); refreshStatus(); roleTimer->start();
	auto *bottom = new QHBoxLayout(); auto *assign = new QPushButton(T("Panel.OpenAssignments"), dialog);
	assign->setObjectName(QStringLiteral("EAOpenAssignments")); bottom->addWidget(assign); bottom->addStretch();
	assign->setToolTip(T("Panel.OpenAssignments.Tooltip"));
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog); bottom->addWidget(buttons); layout->addLayout(bottom);
	auto apply = [initial, controls, backupEnabled, backupFormat, dialog]() mutable {
		QJsonArray tracks;
		for (int i = 0; i < 10; ++i) {
			const auto &c = controls[i]; QJsonObject track;
			track["number"] = i + 7; track["enabled"] = c.enabled->isChecked(); track["name"] = c.name->text();
			track["bitrate"] = c.bitrate->currentData().toInt(); track["main"] = c.main->isChecked();
			track["vertical"] = c.vertical->isChecked(); tracks.append(track);
		}
		initial["tracks"] = tracks; initial["backup_mp3"] = backupEnabled->isChecked(); initial["backup_format"] = backupFormat->text();
		const auto json = QJsonDocument(initial).toJson(QJsonDocument::Compact); char error[1024] = {};
		if (!ea_ui_save(json.constData(), error, sizeof(error))) { showError(dialog, error); return false; } return true;
	};
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [apply, dialog]() mutable { if (apply()) dialog->accept(); });
	QObject::connect(assign, &QPushButton::clicked, dialog, [apply, dialog, parent]() mutable {
		if (!apply()) return; dialog->accept(); QMetaObject::invokeMethod(parent, "on_actionAdvAudioProperties_triggered");
	});
	dialog->show(); dialog->raise(); dialog->activateWindow();
}
