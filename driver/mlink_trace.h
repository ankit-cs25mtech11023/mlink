/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mlink tracepoints: /sys/kernel/tracing/events/mlink/
 * e.g. trace-cmd record -e mlink:* ; bpftrace -e 'tracepoint:mlink:mlink_napi_poll { ... }'
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mlink

#if !defined(_MLINK_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _MLINK_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(mlink_xmit,
	TP_PROTO(u32 len, u32 idx, bool kicked),
	TP_ARGS(len, idx, kicked),
	TP_STRUCT__entry(
		__field(u32, len)
		__field(u32, idx)
		__field(bool, kicked)
	),
	TP_fast_assign(
		__entry->len = len;
		__entry->idx = idx;
		__entry->kicked = kicked;
	),
	TP_printk("len=%u prod=%u kicked=%d", __entry->len, __entry->idx,
		  __entry->kicked)
);

TRACE_EVENT(mlink_kick,
	TP_PROTO(u32 peer, u32 vec),
	TP_ARGS(peer, vec),
	TP_STRUCT__entry(
		__field(u32, peer)
		__field(u32, vec)
	),
	TP_fast_assign(
		__entry->peer = peer;
		__entry->vec = vec;
	),
	TP_printk("peer=%u vec=%u", __entry->peer, __entry->vec)
);

TRACE_EVENT(mlink_irq,
	TP_PROTO(u32 vec),
	TP_ARGS(vec),
	TP_STRUCT__entry(
		__field(u32, vec)
	),
	TP_fast_assign(
		__entry->vec = vec;
	),
	TP_printk("vec=%u", __entry->vec)
);

TRACE_EVENT(mlink_napi_poll,
	TP_PROTO(int budget, int done),
	TP_ARGS(budget, done),
	TP_STRUCT__entry(
		__field(int, budget)
		__field(int, done)
	),
	TP_fast_assign(
		__entry->budget = budget;
		__entry->done = done;
	),
	TP_printk("budget=%d done=%d", __entry->budget, __entry->done)
);

TRACE_EVENT(mlink_rx,
	TP_PROTO(u32 len, u16 proto, u32 idx),
	TP_ARGS(len, proto, idx),
	TP_STRUCT__entry(
		__field(u32, len)
		__field(u16, proto)
		__field(u32, idx)
	),
	TP_fast_assign(
		__entry->len = len;
		__entry->proto = proto;
		__entry->idx = idx;
	),
	TP_printk("len=%u proto=0x%04x cons=%u", __entry->len, __entry->proto,
		  __entry->idx)
);

#endif /* _MLINK_TRACE_H */

/* Must stay outside the include guard. */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE mlink_trace
#include <trace/define_trace.h>
