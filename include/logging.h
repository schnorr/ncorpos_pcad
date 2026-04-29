#ifndef __LOGGING_H_
#define __LOGGING_H_

#define LOG_NONE  0
#define LOG_BASIC 1   /* log overall timing per simulation request   */
#define LOG_FULL  2   /* also log per-iteration worker timings        */

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_BASIC
#endif

#endif /* __LOGGING_H_ */
