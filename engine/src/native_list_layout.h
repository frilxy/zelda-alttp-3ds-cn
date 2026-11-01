#pragma once

// Shared native selector geometry: five rows and a separate page counter.
enum { kNativeListRows=5, kNativeListStep=24, kNativeListCounterY=207 };
static inline int NativeListPages(int count) { return count>0?(count+4)/5:1; }
static inline int NativeListPage(int selected) { return selected>0?selected/5:0; }
static inline int NativeListFirst(int selected) { return NativeListPage(selected)*5; }
static inline int NativeListVisible(int count,int first) { int n=count-first;return n<5?n:5; }
static inline int NativeListRowY(int rows,int row) { return 76+(5-rows)*12+row*24; }
static inline int NativeListStepPage(int selected,int direction,int count) {
  int page=NativeListPage(selected)+direction,pages=NativeListPages(count);
  if(page<0)page=0;
  if(page>=pages)page=pages-1;
  int next=page*5+selected%5;
  return next<count?next:count-1;
}
