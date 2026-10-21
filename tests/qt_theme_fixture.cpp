// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <QApplication>
#include <QPalette>
#include <QDBusConnection>
#include <QPushButton>
#include <QStyleHints>
#include <QTimer>
#include <fstream>

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 1;
  QApplication application(argc, argv);
  QPushButton button("Qt theme fixture");
  button.show();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    output << qgetenv("QT_QPA_PLATFORMTHEME").constData() << '|'
           << static_cast<int>(application.styleHints()->colorScheme()) << '|'
           << application.palette().color(QPalette::Window).name().toStdString() << '|'
           << application.palette().color(QPalette::WindowText).name().toStdString() << '\n';
    output.flush();
  });
  timer.start(100);
  if (argc == 3) {
    if (!QDBusConnection::sessionBus().registerService(QString::fromLocal8Bit(argv[2]))) return 1;
    QTimer::singleShot(1000, &application, &QApplication::quit);
  }
  return application.exec();
}
