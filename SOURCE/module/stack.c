/*
 * Linux内核诊断工具--内核态堆栈公共函数实现
 *
 * Copyright (C) 2020 Alibaba Ltd.
 *
 * 作者: Baoyou Xie <baoyou.xie@linux.alibaba.com>
 *
 * License terms: GNU General Public License (GPL) version 3
 *
 */

#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/tracepoint.h>
#include <trace/events/irq.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <trace/events/napi.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/rbtree.h>

#include "internal.h"

__maybe_unused static struct diag_stack_desc *__find_stack_desc(struct diag_stack_trace *trace,
		unsigned long *trace_buf)
{
	struct rb_node *node = trace->stack_tree.rb_node;

	while (node) {
		struct diag_stack_desc *this =
			container_of(node, struct diag_stack_desc, rb_node);
		int ret;

		ret = diagnose_stack_trace_cmp(trace_buf, this->trace_buf);
		if (ret < 0)
			node = node->rb_left;
		else if (ret > 0)
			node = node->rb_right;
		else
			return this;
	}

	return NULL;
}

struct diag_stack_desc *__diag_stack_desc_find_alloc(unsigned long *trace_buf,
	struct diag_stack_trace *trace)
{
	
	struct diag_stack_desc *stack_desc;
	struct diag_stack_desc *this;
	struct rb_node **node, *parent;

	node = &trace->stack_tree.rb_node;
	parent = NULL;

	while (*node != NULL) {
		int ret;
	
		parent = *node;
		this = container_of(parent, struct diag_stack_desc, rb_node);

		ret = diagnose_stack_trace_cmp(trace_buf, this->trace_buf);

		if (ret < 0)
			node = &parent->rb_left;
		else if (ret > 0)
			node = &parent->rb_right;
		else {
			return this;
		}
	}

	stack_desc = kmalloc(sizeof(struct diag_stack_desc), GFP_ATOMIC | __GFP_ZERO);
	if (stack_desc) {
		memcpy(stack_desc->trace_buf, trace_buf,
				sizeof(stack_desc->trace_buf));
		/* Add new node and rebalance tree. */
		rb_link_node(&stack_desc->rb_node, parent, node);
		rb_insert_color(&stack_desc->rb_node, &trace->stack_tree);
	}

	return stack_desc;
}

struct diag_stack_desc *diag_stack_desc_find_alloc(struct diag_stack_trace *trace)
{
	unsigned long trace_buf[BACKTRACE_DEPTH];
	struct diag_stack_desc *stack_desc;

	diagnose_save_stack_trace(current, trace_buf);
	spin_lock(&trace->tree_lock);
	stack_desc = __diag_stack_desc_find_alloc(trace_buf, trace);
	spin_unlock(&trace->tree_lock);

	return stack_desc;
}

int diag_dump_trace_stack(struct diag_stack_trace *trace)
{
	int count = 0;
	int i;
	struct diag_stack_desc *stack_desc;
	char str[KSYM_SYMBOL_LEN];
	struct rb_node *node;

	spin_lock(&trace->tree_lock);

	for (node = rb_first(&trace->stack_tree); node; node = rb_next(node)) {
		stack_desc = rb_entry(node, struct diag_stack_desc, rb_node);

		if (atomic64_read(&stack_desc->hit_count) == 0)
			continue;

		count += atomic64_read(&stack_desc->hit_count);
		diag_trace_printk("------------------------------------------\n");
		diag_trace_printk("hit count %llu\n", (u64)atomic64_read(&stack_desc->hit_count));
		diag_trace_printk("orders:\n");
		for (i = 0; i < MAX_ORDER; i++)
			diag_trace_printk("    %2d: %d\n", i, stack_desc->alloc_count_orders[i]);
		for (i = 0; i < BACKTRACE_DEPTH; i++) {
			if (STACK_IS_END(stack_desc->trace_buf[i]))
				break;

			sprint_symbol(str, stack_desc->trace_buf[i]);
			diag_trace_printk("\t%s\n", str);
		}
	}

	spin_unlock(&trace->tree_lock);

	return count;
}

int diag_printk_trace_stack(struct diag_stack_trace *trace)
{
	int count = 0;
	struct rb_node *node;
	int i;
	struct diag_stack_desc *stack_desc;
	char str[KSYM_SYMBOL_LEN];

	spin_lock(&trace->tree_lock);

	for (node = rb_first(&trace->stack_tree); node; node = rb_next(node)) {
		stack_desc = rb_entry(node, struct diag_stack_desc, rb_node);

		if (atomic64_read(&stack_desc->hit_count) == 0)
			continue;

		count += atomic64_read(&stack_desc->hit_count);
		printk("------------------------------------------\n");
		printk("hit count %llu\n", (u64)atomic64_read(&stack_desc->hit_count));
		for (i = 0; i < BACKTRACE_DEPTH; i++) {
			if (STACK_IS_END(stack_desc->trace_buf[i]))
				break;

			sprint_symbol(str, stack_desc->trace_buf[i]);
			printk("\t%s\n", str);
		}
	}

	spin_unlock(&trace->tree_lock);

	return count;
}

void diag_init_trace_stack(struct diag_stack_trace *trace)
{
	trace->stack_tree = RB_ROOT;
	trace->tree_lock = __SPIN_LOCK_UNLOCKED(trace->tree_lock);
}

void diag_cleanup_trace_stack(struct diag_stack_trace *trace)
{
	struct diag_stack_desc *stack_desc;
	struct rb_node *node;
	struct list_head stack_list;

	INIT_LIST_HEAD(&stack_list);
	spin_lock(&trace->tree_lock);
	for (node = rb_first(&trace->stack_tree); node; node = rb_next(node)) {
		stack_desc = rb_entry(node, struct diag_stack_desc, rb_node);

		INIT_LIST_HEAD(&stack_desc->list);
		list_add_tail(&stack_desc->list, &stack_list);
	}
	trace->stack_tree = RB_ROOT;
	spin_unlock(&trace->tree_lock);

	while (!list_empty(&stack_list)) {
		stack_desc = list_first_entry(&stack_list, struct diag_stack_desc, list);
        
		list_del_init(&stack_desc->list);
		kfree(stack_desc);
	}
}
