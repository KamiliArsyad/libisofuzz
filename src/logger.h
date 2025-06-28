#ifndef LOGGER_H
#define LOGGER_H

#include <string>

void logger_init();
void logger_shutdown();
void logger_log_line(const std::string& line);

#endif // LOGGER_H
