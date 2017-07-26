/*
 * list.h
 *
 * Description: simple double link list used all over
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */
#ifndef __LIST_H__
#define __LIST_H__

#ifndef __LIST_HEAD_DEFINED
struct list_head {
	struct list_head *next, *prev;
};
#define __LIST_HEAD_DEFINED
#endif

static inline void list_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void _list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}
static inline void list_add(struct list_head *new, struct list_head *head)
{
	_list_add(new, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
}
static inline void list_del_init(struct list_head *entry)
{
	list_del(entry);
	list_init(entry);
}
static inline int list_empty(const struct list_head *head)
{
	return  (head->next == head);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
       _list_add(new, head->prev, head);
}

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({			\
	typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#define list_for_each_entry(pos, head, member)				\
	for (pos = container_of((head)->next, typeof(*pos), member);	\
	     &pos->member != (head); 	\
	     pos = container_of(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, t, head, member)			\
	for (pos = container_of((head)->next, typeof(*pos), member),		\
		t = container_of(pos->member.next, typeof(*pos), member);	\
	     &pos->member != (head); 						\
	     pos = t, t = container_of(t->member.next, typeof(*t), member))

#endif /* ndef __LIST_H__ */
