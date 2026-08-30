#include <linux/compiler.h>
#include <linux/printk.h>

#define RWNX_FN_ENTRY_STR ">>> %s()\n", __func__


/* message levels */
#define LOGERROR		0x0001
#define LOGINFO			0x0002
#define LOGTRACE		0x0004
#define LOGDEBUG		0x0008
#define LOGDATA			0x0010


extern int aicwf_dbg_level;
void rwnx_data_dump(char* tag, void* data, unsigned long len);

#define AICWF_LOG_PREFIX KBUILD_MODNAME ": AICWFDBG("

#define __AICWF_PRINT_LOGERROR(args, arg...) \
	pr_err(AICWF_LOG_PREFIX "LOGERROR)\t" args, ##arg)
#define __AICWF_PRINT_LOGINFO(args, arg...) \
	pr_info(AICWF_LOG_PREFIX "LOGINFO)\t" args, ##arg)
#define __AICWF_PRINT_LOGTRACE(args, arg...) \
	printk(KERN_DEBUG AICWF_LOG_PREFIX "LOGTRACE)\t" args, ##arg)
#define __AICWF_PRINT_LOGDEBUG(args, arg...) \
	printk(KERN_DEBUG AICWF_LOG_PREFIX "LOGDEBUG)\t" args, ##arg)
#define __AICWF_PRINT_LOGDATA(args, arg...) \
	printk(KERN_DEBUG AICWF_LOG_PREFIX "LOGDATA)\t" args, ##arg)
#define __AICWF_PRINT_RATELIMITED_LOGERROR(args, arg...) \
	pr_err_ratelimited(AICWF_LOG_PREFIX "LOGERROR)\t" args, ##arg)
#define __AICWF_PRINT_RATELIMITED_LOGINFO(args, arg...) \
	pr_info_ratelimited(AICWF_LOG_PREFIX "LOGINFO)\t" args, ##arg)
#define __AICWF_PRINT_RATELIMITED_LOGTRACE(args, arg...) \
	printk_ratelimited(KERN_DEBUG AICWF_LOG_PREFIX "LOGTRACE)\t" args, ##arg)
#define __AICWF_PRINT_RATELIMITED_LOGDEBUG(args, arg...) \
	printk_ratelimited(KERN_DEBUG AICWF_LOG_PREFIX "LOGDEBUG)\t" args, ##arg)
#define __AICWF_PRINT_RATELIMITED_LOGDATA(args, arg...) \
	printk_ratelimited(KERN_DEBUG AICWF_LOG_PREFIX "LOGDATA)\t" args, ##arg)
#define AICWFDBG(level, args, arg...) \
do { \
	if (READ_ONCE(aicwf_dbg_level) & (level)) \
		__AICWF_PRINT_##level(args, ##arg); \
} while (0)

#define AICWFDBG_RATELIMITED(level, args, arg...) \
do { \
	if (READ_ONCE(aicwf_dbg_level) & (level)) \
		__AICWF_PRINT_RATELIMITED_##level(args, ##arg); \
} while (0)

#define RWNX_DBG(fmt, ...)	\
do {	\
	if (READ_ONCE(aicwf_dbg_level) & LOGTRACE) \
		__AICWF_PRINT_LOGTRACE(fmt, ##__VA_ARGS__); \
} while (0)


#if 0
#define RWNX_DBG(fmt, ...)	\
	do {	\
		if (aicwf_dbg_level & LOGTRACE) {	\
			printk(AICWF_LOG"LOGTRACE"")\t" fmt, ##__VA_ARGS__); \
		}	\
	} while (0)
#define AICWFDBG(args, level)	\
do {	\
	if (aicwf_dbg_level & level) {	\
		printk(AICWF_LOG"(%s)\t" ,#level);	\
		printf args;	\
	}	\
} while (0)
#endif
