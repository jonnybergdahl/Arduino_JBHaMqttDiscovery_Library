#pragma once

#include <stddef.h>

/**
 * @brief Log level enumeration.
 */
enum LogLevel {
  LOG_LEVEL_NONE = 0,    /**< No logging */
  LOG_LEVEL_ERROR,       /**< Critical errors */
  LOG_LEVEL_WARNING,     /**< Warnings */
  LOG_LEVEL_INFO,        /**< Informational messages */
  LOG_LEVEL_DEBUG,       /**< Debug information */
  LOG_LEVEL_TRACE        /**< High-frequency trace data */
};

/**
 * @brief Minimal logger interface used when jblogger.h is missing.
 */
class JBLogger {
public:
  /** @brief Constructor. @param moduleName Name of the module. */
  JBLogger(const char* moduleName, LogLevel level = LogLevel::LOG_LEVEL_TRACE) {}
  /** @brief Log a trace message. @param format Format string. */
  virtual void trace(const char* format, ...) {}
  /** @brief Log a debug message. @param format Format string. */
  virtual void debug(const char* format, ...) {}
  /** @brief Log an info message. @param format Format string. */
  virtual void info(const char* format, ...) {}
  /** @brief Log a warning message. @param format Format string. */
  virtual void warning(const char* format, ...) {}
  /** @brief Log an error message. @param format Format string. */
  virtual void error(const char* format, ...) {}
  /** @brief Set the log level. @param level Minimum log level to log. */
  virtual void setLogLevel(LogLevel level) {}
};
