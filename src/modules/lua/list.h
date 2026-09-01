#ifndef _LUA_LIST_H_
#define _LUA_LIST_H_

typedef struct List List;
typedef struct ListIter ListIter;

List *list_create(void);
void list_destroy(List *list);
void list_add(List *list, void *val);
int list_length(List *list);

ListIter *list_get_iter(List *list);
void *list_iter_next(ListIter *iter);
void list_release_iter(ListIter *iter);

#endif /* _LUA_LIST_H_ */
