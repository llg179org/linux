/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_smd_rpm

#if !defined(_TRACE_QCOM_SMD_RPM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QCOM_SMD_RPM_H

#include <linux/tracepoint.h>
#include <linux/soc/qcom/smd-rpm.h>

/*
 * The RPM keeps two vote sets, active and sleep, and a resource voted only in
 * the active set keeps that vote in force while the application processor is
 * power collapsed. So "which resources did we vote for sleep, and at what
 * value" decides how deep the SoC can go, and there is no way to ask the RPM
 * itself - it exposes aggregate results (the vlow/vmin sleep statistics) but
 * not the votes behind them. What the processor sent is observable, and this
 * is where it is sent.
 *
 * The resource type is four ASCII characters in a u32; it is emitted as the
 * raw word and printed as characters, because the names ("ldoa", "smpa",
 * "bimc") are how the vendor documentation refers to them.
 */
TRACE_EVENT(qcom_rpm_smd_write,

	TP_PROTO(int state, u32 type, u32 id, const void *buf, size_t count),

	TP_ARGS(state, type, id, buf, count),

	TP_STRUCT__entry(
		__field(int, state)
		__field(u32, type)
		__field(u32, id)
		__field(size_t, count)
		__dynamic_array(u8, payload, count)
	),

	TP_fast_assign(
		__entry->state = state;
		__entry->type = type;
		__entry->id = id;
		__entry->count = count;
		memcpy(__get_dynamic_array(payload), buf, count);
	),

	TP_printk("%s %c%c%c%c/%u len=%zu %s",
		  __entry->state == QCOM_SMD_RPM_SLEEP_STATE ? "sleep" : "active",
		  (char)(__entry->type & 0xff),
		  (char)((__entry->type >> 8) & 0xff),
		  (char)((__entry->type >> 16) & 0xff),
		  (char)((__entry->type >> 24) & 0xff),
		  __entry->id, __entry->count,
		  __print_hex(__get_dynamic_array(payload), __entry->count))
);

#endif /* _TRACE_QCOM_SMD_RPM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
