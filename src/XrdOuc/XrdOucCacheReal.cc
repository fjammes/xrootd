/******************************************************************************/
/*                                                                            */
/*                    X r d O u c C a c h e R e a l . c c                     */
/*                                                                            */
/* (c) 2011 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                            All Rights Reserved                             */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC02-76-SFO0515 with the Department of Energy              */
/******************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <values.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "XrdOuc/XrdOucCacheData.hh"
#include "XrdSys/XrdSysHeaders.hh"
  
/******************************************************************************/
/*                           C o n s t r u c t o r                            */
/******************************************************************************/

void *XrdOucCacheRealPRXeq(void *parg)
{   XrdOucCacheReal *cP = (XrdOucCacheReal *)parg;
    cP->PreRead();
    return (void *)0;
}
  
XrdOucCacheReal::XrdOucCacheReal(int &rc, XrdOucCache::Parms      &ParmV,
                                          XrdOucCacheIO::aprParms *aprP)
                : Slots(0), Slash(0), Base((char *)MAP_FAILED), Dbg(0), Lgs(0),
                  AZero(0), Attached(0), prFirst(0), prLast(0),
                  prReady(0), prStop(0), prNum(0)
{
   size_t Bytes;
   int n, isServ = ParmV.Options & isServer;

// Copy over options
//
   rc = ENOMEM;
   Options = ParmV.Options;
   if (Options & Debug)    Lgs = Dbg = (Options & Debug);
   if (Options & logStats) Lgs = 1;

// Establish maximum number of attached files
//
   if (ParmV.MaxFiles <= 0) maxFiles = (isServ ? 16384 : 256);
      else {maxFiles = (ParmV.MaxFiles > 32764 ? 32764 : ParmV.MaxFiles);
            maxFiles = maxFiles/sizeof(int)*sizeof(int);
            if (!maxFiles) maxFiles = 256;
           }

// Adjust segment size to be a power of two and atleast 4k.
//
        if (ParmV.PageSize <= 0) SegSize = 16384;
   else if (!(SegSize = ParmV.PageSize & ~0xfff)) SegSize = 4096;
   else if (SegSize > 16*1024*1024) SegSize = 16*1024*1024;
   SegShft = 0; n = SegSize-1;
   while(n) {SegShft++; n = n >> 1;}
   SegSize = 1<<SegShft;

// The max to cache is also adjusted accrodingly based on segment size.
//
   OffMask = SegSize-1;
   SegCnt = (ParmV.CacheSize > 0 ? ParmV.CacheSize : 104857600)/SegSize;
   if (SegCnt < 256) SegCnt = 256;
   if (ParmV.Max2Cache < SegSize) maxCache = SegSize;
      else maxCache = ParmV.Max2Cache/SegSize*SegSize;
   SegFull = (Options & isServer ? XrdOucCacheSlot::lenMask : SegSize);

// Allocate the cache plus the cache hash table
//
   Bytes = static_cast<size_t>(SegSize)*SegCnt;
   Base = (char *)mmap(0, Bytes + SegCnt*sizeof(int), PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
   if (Base == MAP_FAILED) {rc = errno; return;}
   Slash = (int *)(Base + Bytes); HNum = SegCnt/2*2-1;

// Now allocate the actual slots. We add additional slots to map files. These
// do not have any memory backing but serve as anchors for memory mappings.
//
   if (!(Slots = new XrdOucCacheSlot[SegCnt+maxFiles])) return;
   XrdOucCacheSlot::Init(Slots, SegCnt);

// Set pointers to be able to keep track of CacheIO objects and map them to
// CacheData objects. The hash table will be the first page of slot memory.
//
   hTab = (int *)Base;
   hMax = SegSize/sizeof(int);
   sBeg = sFree = SegCnt;
   sEnd = SegCnt + maxFiles;

// Now iniialize the slots to be used for the CacheIO objects
//
   for (n = sBeg; n < sEnd; n++)
       {Slots[n].Own.Next = Slots[n].Own.Prev = n;
        Slots[n].HLink = n+1;
       }
   Slots[sEnd-1].HLink = 0;

// Setup the pre-readers if pre-read is enabled
//
   if (Options & canPreRead)
      {pthread_t tid;
       n = (Options & isServer ? 9 : 3);
       while(n--)
            {if (XrdSysThread::Run(&tid, XrdOucCacheRealPRXeq, (void *)this,
                                   0, "Prereader")) break;
             prNum++;
            }
       if (aprP && prNum) XrdOucCacheData::setAPR(aprDefault, *aprP, SegSize);
      }

// All done
//
   rc = 0;
}

/******************************************************************************/
/*                            D e s t r u c t o r                             */
/******************************************************************************/

XrdOucCacheReal::~XrdOucCacheReal()
{
// Wait for all attachers to go away
//
   CMutex.Lock();
   if (Attached)
      {XrdSysSemaphore aZero(0);
       AZero = &aZero;
       CMutex.UnLock();
       aZero.Wait();
       CMutex.Lock();
      }

// If any preread threads exist, then stop them now
//
   prMutex.Lock();
   if (prNum)
      {XrdSysSemaphore prDone(0);
       prStop = &prDone;
       prReady.Post();
       prMutex.UnLock();
       prDone.Wait();
       prMutex.Lock();
      }

// Delete the slots
//
   delete Slots; Slots = 0;

// Unmap cache memory and associated hash table
//
   if (Base != MAP_FAILED)
      {munmap(Base, static_cast<size_t>(SegSize)*SegCnt);
       Base = (char *)(MAP_FAILED);
      }

// Release all locks, we are done
//
   prMutex.UnLock();
   CMutex.UnLock();
}

/******************************************************************************/
/*                                A t t a c h                                 */
/******************************************************************************/

XrdOucCacheIO *XrdOucCacheReal::Attach(XrdOucCacheIO *ioP, int Opts)
{
   static int Inst = 0;
   XrdSysMutexHelper Monitor(CMutex);
   XrdOucCacheData  *dP;
   int Cnt, Fnum = 0, theOpts = Opts & (optRW | optADB);

// Check if we are being deleted
//
   if (AZero) {errno = ECANCELED; return ioP;}

// Setup structured/unstructured option
//
   if (Opts & (optFIS | optFIU))  theOpts |= (Opts & optFIU ? 0 : optFIS);
      else if (Options & isStructured) theOpts |= optFIS;

// Get an entry in the filename table.
//
   if (!(Cnt = ioAdd(ioP, Fnum)))
      {errno = EMFILE;
       return ioP;
      }

// If this is the first addition then we need to get a new CacheData object.
// Otherwise, simply reuse the previous cache data object.
//
   if (Cnt != 1) dP = Slots[Fnum].Status.Data;
      else {long long vNum =  static_cast<long long>(Fnum-SegCnt) <<  Shift
                           | (static_cast<long long>(Inst) << (Shift - 16));
            Inst = (Inst+1) & 0xffff;
            if ((dP = new XrdOucCacheData(this, ioP, vNum, theOpts)))
               {Attached++; Slots[Fnum].Status.Data = dP;}
           }

// Some debugging
//
   if (Dbg) cerr <<"Cache: Attached " <<Cnt <<'/' <<Attached <<' '
                 <<std::hex << Fnum <<std::dec <<' ' <<ioP->Path() <<endl;

// All done
//
   if (!dP) {errno = ENOMEM; return ioP;}
   return (XrdOucCacheIO *)dP;
}
  
/******************************************************************************/
/*                                D e t a c h                                 */
/******************************************************************************/

int XrdOucCacheReal::Detach(XrdOucCacheIO *ioP)
{
   XrdSysMutexHelper Monitor(CMutex);
   XrdOucCacheSlot  *sP, *oP;
   int sNum, Fnum, Free = 0, Faults = 0;

// Now we delete this CacheIO from the cache set and see if its still ref'd.
//
   sNum = ioDel(ioP, Fnum);
   if (!sNum || sNum > 1) return 0;

// We will be deleting the CacheData object. So, we need to recycle its slots.
//
   oP = &Slots[Fnum];
   while(oP->Own.Next != Fnum)
        {sP = &Slots[oP->Own.Next];
         sP->Owner(Slots);
         if (sP->Contents < 0 || sP->Status.LRU.Next < 0) Faults++;
            else {sP->Hide(Slots, Slash, sP->Contents%HNum);
                  sP->Pull(Slots);
                  sP->unRef(Slots);
                  Free++;
                 }
        }

// Reduce attach count and check if the cache is being deleted
//
   Attached--;
   if (AZero && Attached <= 0) AZero->Post();

// Issue debugging message
//
   if (Dbg) cerr <<"Cache: " <<Attached <<" att; rel " <<Free <<" slots; "
                 <<Faults <<" Faults; " <<std::hex << Fnum <<std::dec <<' '
                 <<ioP->Path() <<endl;

// All done, tell the caller to delete itself
//
   return 1;
}

/******************************************************************************/
/*                                  e M s g                                   */
/******************************************************************************/
  
void XrdOucCacheReal::eMsg(const char *Path, const char *What, long long xOff,
                       int xAmt, int eCode)
{
   char Buff[128];

   if (Dbg)
      {sprintf(Buff, "Cache: Error %d %s %d bytes at %lld; path=",
                     eCode, What, xAmt, xOff);
       cerr <<Buff <<Path <<endl;
      }
}
  
/******************************************************************************/
/*                                   G e t                                    */
/******************************************************************************/
  
char *XrdOucCacheReal::Get(XrdOucCacheIO *ioP, long long lAddr,
                       int &rAmt, int &noIO)
{
   XrdSysMutexHelper Monitor(CMutex);
   XrdOucCacheSlot::ioQ *Waiter;
   XrdOucCacheSlot *sP;
   int nUse, Fnum, Slot, segHash = lAddr%HNum;
   char *cBuff;

// See if we have this logical address in the cache. Check if the page is in
// transit and, if so, wait for it to arrive before proceeding.
//
   noIO = 1;
   if (Slash[segHash]
   &&  (Slot = XrdOucCacheSlot::Find(Slots, lAddr, Slash[segHash])))
      {sP = &Slots[Slot];
       if (sP->Count & XrdOucCacheSlot::inTrans)
          {XrdSysSemaphore ioSem(0);
           XrdOucCacheSlot::ioQ ioTrans(sP->Status.waitQ, &ioSem);
           sP->Status.waitQ = &ioTrans;
           if (Dbg > 1) cerr <<"Cache: Wait slot " <<Slot <<endl;
           CMutex.UnLock(); ioSem.Wait(); CMutex.Lock();
           if (sP->Contents != lAddr) {rAmt = -EIO; return 0;}
          } else {
            if (sP->Status.inUse < 0) sP->Status.inUse--;
               else {sP->Pull(Slots); sP->Status.inUse = -1;}
          }
       rAmt = (sP->Count < 0 ? sP->Count & XrdOucCacheSlot::lenMask : SegSize);
       if (sP->Count & XrdOucCacheSlot::isNew)
          {noIO = -1; sP->Count &= ~XrdOucCacheSlot::isNew;}
       if (Dbg > 2) cerr <<"Cache: Hit slot " <<Slot <<" sz " <<rAmt <<" nio "
                         <<noIO <<" uc " <<sP->Status.inUse <<endl;
       return Base+(static_cast<long long>(Slot)*SegSize);
      }

// Page is not here. If no allocation wanted or we cannot obtain a free slot
// return and indicate there is no associated cache page.
//
   if (!ioP || !(Slot = Slots[Slots->Status.LRU.Next].Pull(Slots)))
      {rAmt = -ENOMEM; return 0;}

// Remove ownership over this slot and remove it from the hash table
//
   sP = &Slots[Slot];
   if (sP->Contents >= 0)
      {if (sP->Own.Next != Slot) sP->Owner(Slots);
       sP->Hide(Slots, Slash, sP->Contents%HNum);
      }

// Read the data into the buffer
//
   sP->Count |= XrdOucCacheSlot::inTrans;
   sP->Status.waitQ = 0;
   CMutex.UnLock();
   cBuff = Base+(static_cast<long long>(Slot)*SegSize);
   rAmt = ioP->Read(cBuff, (lAddr & Strip) << SegShft, SegSize);
   CMutex.Lock();

// Post anybody waiting for this slot. We hold the cache lock which will give us
// time to complete the slot definition before the waiting thread looks at it.
//
   nUse = -1;
   while((Waiter = sP->Status.waitQ))
        {sP->Status.waitQ = sP->Status.waitQ->Next;
         sP->Status.waitQ->ioEnd->Post();
         nUse--;
        }

// If I/O succeeded, reinitialize the slot. Otherwise, return free it up
//
   noIO = 0;
   if (rAmt >= 0)
      {sP->Contents   = lAddr;
       sP->HLink      = Slash[segHash];
       Slash[segHash] = Slot;
       Fnum = (lAddr >> Shift) + SegCnt;
       Slots[Fnum].Owner(Slots, sP);
       sP->Count = (rAmt == SegSize ? SegFull : rAmt|XrdOucCacheSlot::isShort);
       sP->Status.inUse = nUse;
       if (Dbg > 2) cerr <<"Cache: Miss slot " <<Slot <<" sz "
                         <<(sP->Count & XrdOucCacheSlot::lenMask) <<endl;
      } else {
       eMsg(ioP->Path(), "reading", (lAddr & Strip) << SegShft, SegSize, rAmt);
       cBuff = 0;
       sP->Contents = -1;
       sP->unRef(Slots);
      }

// Return the associated buffer or zero, as per above
//
   return cBuff;
}

/******************************************************************************/
/*                                 i o A d d                                  */
/******************************************************************************/
  
int XrdOucCacheReal::ioAdd(XrdOucCacheIO *KeyVal, int &iNum)
{
   int hip, phip, hent = ioEnt(KeyVal);

// Look up the entry. If found, return it.
//
   if ((hip = hTab[hent]) && (hip = ioLookup(phip, hip, KeyVal)))
      {iNum = hip; return ++Slots[hip].Count;}

// Add the entry
//
   if ((hip = sFree))
      {sFree = Slots[sFree].HLink;
       Slots[hip].File(KeyVal, hTab[hent]);
       hTab[hent] = hip;
      }

// Return information to the caller
//
   iNum = hip;
   return (hip ? 1 : 0);
}
  
/******************************************************************************/
/*                                 i o D e l                                  */
/******************************************************************************/
  
int XrdOucCacheReal::ioDel(XrdOucCacheIO *KeyVal, int &iNum)
{
   int cnt, hip, phip, hent = ioEnt(KeyVal);

// Look up the entry.
//
   if (!(hip = hTab[hent]) || !(hip = ioLookup(phip, hip, KeyVal))) return 0;
   iNum = hip;

// Delete the item, if need be, and return
//
   cnt = --(Slots[hip].Count);
   if (cnt <= 0)
      {if (phip) Slots[phip].HLink = Slots[hip].HLink;
          else   hTab[hent]        = Slots[hip].HLink;
       Slots[hip].HLink = sFree; sFree = hip;
      }
   return (cnt < 0 ? 1 : cnt+1);
}

/******************************************************************************/
/*                               P r e R e a d                                */
/******************************************************************************/
  
void XrdOucCacheReal::PreRead()
{
   prTask *prP;

// Simply wait and dispatch elements
//
   if (Dbg) cerr <<"Cache: preread thread started; now " <<prNum <<endl;
   while(1)
        {prReady.Wait();
         prMutex.Lock();
         if (prStop) break;
         if ((prP = prFirst))
            {if (!(prFirst = prP->Next)) prLast = 0;
             prMutex.UnLock();
             prP->Data->Preread();
            } else prMutex.UnLock();
        }

// The cache is being deleted, wind down the prereads
//
   prNum--;
   if (prNum > 0) prReady.Post();
      else        prStop->Post();
   if (Dbg) cerr <<"Cache: preread thread exited; left " <<prNum <<endl;
   prMutex.UnLock();
}

/******************************************************************************/

void XrdOucCacheReal::PreRead(XrdOucCacheReal::prTask *prReq)
{

// Place this element on the queue
//
   prMutex.Lock();
   if (prLast) {prLast->Next = prReq; prLast = prReq;}
      else      prLast = prFirst = prReq;
   prReq->Next = 0;

// Tell a pre-reader that something is ready
//
   prReady.Post();
   prMutex.UnLock();
}

/******************************************************************************/
/*                                   R e f                                    */
/******************************************************************************/
  
int XrdOucCacheReal::Ref(char *Addr, int rAmt, int sFlags)
{
    XrdOucCacheSlot *sP = &Slots[(Addr-Base)>>SegShft];
    int eof = 0;

// Indicate how much data was not yet referenced
//
   CMutex.Lock();
   if (sP->Contents >= 0)
      {if (sP->Count < 0) eof = 1;
       sP->Status.inUse++;
       if (sP->Status.inUse < 0)
          {if (sFlags) sP->Count |= sFlags;
              else if (!eof && (sP->Count -= rAmt) < 0) sP->Count = 0;
          } else {
           if (sFlags) {sP->Count |= sFlags;                 sP->reRef(Slots);}
              else {     if (sP->Count & XrdOucCacheSlot::isSUSE)
                                                             sP->unRef(Slots);
                    else if (eof || (sP->Count -= rAmt) > 0) sP->reRef(Slots);
                    else   {sP->Count = SegSize/2;           sP->unRef(Slots);}
                   }
          }
      } else eof = 1;

// All done
//
   if (Dbg > 2) cerr <<"Cache: Ref " <<std::hex <<sP->Contents <<std::dec
                     << " slot " <<((Addr-Base)>>SegShft)
                     <<" sz " <<(sP->Count & XrdOucCacheSlot::lenMask)
                     <<" uc " <<sP->Status.inUse <<endl;
   CMutex.UnLock();
   return !eof;
}

/******************************************************************************/
/*                                 T r u n c                                  */
/******************************************************************************/

void XrdOucCacheReal::Trunc(XrdOucCacheIO *ioP, long long lAddr)
{
   XrdSysMutexHelper Monitor(CMutex);
   XrdOucCacheSlot  *sP, *oP;
   int sNum, Free = 0, Left = 0, Fnum = (lAddr >> Shift) + SegCnt;

// We will be truncating CacheData pages. So, we need to recycle those slots.
//
   oP = &Slots[Fnum]; sP = &Slots[oP->Own.Next];
   while(oP != sP)
        {sNum = sP->Own.Next;
         if (sP->Contents < lAddr) Left++;
            else {sP->Owner(Slots);
                  sP->Hide(Slots, Slash, sP->Contents%HNum);
                  sP->Pull(Slots);
                  sP->unRef(Slots);
                  Free++;
                 }
         sP = &Slots[sNum];
        }

// Issue debugging message
//
   if (Dbg) cerr <<"Cache: Trunc " <<Free <<" slots; "
                 <<Left <<" Left; " <<std::hex << Fnum <<std::dec <<' '
                 <<ioP->Path() <<endl;
}
  
/******************************************************************************/
/*                                   U p d                                    */
/******************************************************************************/
  
void XrdOucCacheReal::Upd(char *Addr, int wLen, int wOff)
{
    XrdOucCacheSlot *sP = &Slots[(Addr-Base)>>SegShft];

// Check if we extended a short page
//
   CMutex.Lock();
   if (sP->Count < 0)
      {int theLen = sP->Count & XrdOucCacheSlot::lenMask;
       if (wLen + wOff > theLen)
          sP->Count = (wLen+wOff) | XrdOucCacheSlot::isShort;
      }

// Adjust the reference counter and if no references, place on the LRU chain
//
   sP->Status.inUse++;
   if (sP->Status.inUse >= 0) sP->reRef(Slots);

// All done
//
   if (Dbg > 2) cerr <<"Cache: Upd " <<std::hex <<sP->Contents <<std::dec
                     << " slot " <<((Addr-Base)>>SegShft)
                     <<" sz " <<(sP->Count & XrdOucCacheSlot::lenMask)
                     <<" uc " <<sP->Status.inUse <<endl;
   CMutex.UnLock();
}
