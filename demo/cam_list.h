/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifndef _CAM_LIST_H_
#define _CAM_LIST_H_
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *LIST_HANDLE;
typedef struct LIST_ITERATOR { void *const item; } LIST_ITERATOR_S, *LIST_ITERATOR_S_PTR;
typedef bool (*List_OnExternalFind)(const void *item, const void *condition);
typedef bool (*List_OnForeach)(void *item, void *privateData);
LIST_HANDLE List_Create(bool allowRepeatItem);
bool List_Destroy(LIST_HANDLE handle);
ssize_t List_GetSize(LIST_HANDLE handle);
bool List_IsEmpty(LIST_HANDLE handle);
LIST_ITERATOR_S *List_Push_With_Cond(LIST_HANDLE handle, const void *item);
LIST_ITERATOR_S *List_Push(LIST_HANDLE handle, const void *item);
void *List_Pop_With_Cond(LIST_HANDLE handle);
void *List_Pop(LIST_HANDLE handle);
LIST_ITERATOR_S *List_EraseByIterator(LIST_ITERATOR_S *iterator);
bool List_EraseByItem(LIST_HANDLE handle, const void *item);
LIST_ITERATOR_S *List_Find(LIST_HANDLE handle, const void *item);
bool List_Clear(LIST_HANDLE handle);
#ifdef __cplusplus
}
#endif
#endif
