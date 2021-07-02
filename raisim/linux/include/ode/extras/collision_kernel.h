/*************************************************************************
 *                                                                       *
 * Open Dynamics Engine, Copyright (C) 2001-2003 Russell L. Smith.       *
 * All rights reserved.  Email: russ@q12.org   Web: www.q12.org          *
 *                                                                       *
 * This library is free software; you can redistribute it and/or         *
 * modify it under the terms of EITHER:                                  *
 *   (1) The GNU Lesser General Public License as published by the Free  *
 *       Software Foundation; either version 2.1 of the License, or (at  *
 *       your option) any later version. The text of the GNU Lesser      *
 *       General Public License is included with this library in the     *
 *       file LICENSE.TXT.                                               *
 *   (2) The BSD-style license that is included with this library in     *
 *       the file LICENSE-BSD.TXT.                                       *
 *                                                                       *
 * This library is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the files    *
 * LICENSE.TXT and LICENSE-BSD.TXT for more details.                     *
 *                                                                       *
 *************************************************************************/

/*

internal data structures and functions for collision detection.

*/

#ifndef _ODE_COLLISION_KERNEL_H_
#define _ODE_COLLISION_KERNEL_H_

#include <ode/common.h>
#include <ode/contact.h>
#include <ode/collision.h>
#include <ode/objects.h>
#include "odetls.h"
#include "common.h"


//****************************************************************************
// constants and macros

// mask for the number-of-contacts field in the dCollide() flags parameter
#define NUMC_MASK (0xffff)

#define IS_SPACE(geom) \
    dIN_RANGE((geom)->type, dFirstSpaceClass, dLastSpaceClass + 1)

#define CHECK_NOT_LOCKED(space) \
    dUASSERT ((space) == NULL || (space)->lock_count == 0, \
        "Invalid operation for locked space")


//****************************************************************************
// geometry object base class


// geom flags.
//
// GEOM_DIRTY means that the space data structures for this geom are
// potentially not up to date. NOTE THAT all space parents of a dirty geom
// are themselves dirty. this is an invariant that must be enforced.
//
// GEOM_AABB_BAD means that the cached AABB for this geom is not up to date.
// note that GEOM_DIRTY does not imply GEOM_AABB_BAD, as the geom might
// recalculate its own AABB but does not know how to update the space data
// structures for the space it is in. but GEOM_AABB_BAD implies GEOM_DIRTY.
// the valid combinations are: 
//		0
//		GEOM_DIRTY
//		GEOM_DIRTY|GEOM_AABB_BAD
//		GEOM_DIRTY|GEOM_AABB_BAD|GEOM_POSR_BAD

enum {
    GEOM_DIRTY	= 1,    // geom is 'dirty', i.e. position unknown
    GEOM_POSR_BAD = 2,    // geom's final posr is not valid
    GEOM_AABB_BAD	= 4,    // geom's AABB is not valid
    GEOM_PLACEABLE = 8,   // geom is placeable
    GEOM_ENABLED = 16,    // geom is enabled
    GEOM_ZERO_SIZED = 32, // geom is zero sized

    GEOM_ENABLE_TEST_MASK = GEOM_ENABLED | GEOM_ZERO_SIZED,
    GEOM_ENABLE_TEST_VALUE = GEOM_ENABLED,

    // Ray specific
    RAY_FIRSTCONTACT = 0x10000,
    RAY_BACKFACECULL = 0x20000,
    RAY_CLOSEST_HIT  = 0x40000
};

enum dxContactMergeOptions {
    DONT_MERGE_CONTACTS,
    MERGE_CONTACT_NORMALS,
    MERGE_CONTACTS_FULLY
};


// geometry object base class. pos and R will either point to a separately
// allocated buffer (if body is 0 - pos points to the dxPosR object) or to
// the pos and R of the body (if body nonzero).
// a dGeomID is a pointer to this object.

struct dxGeom : public dBase {
    int type;		// geom type number, set by subclass constructor
    int gflags;		// flags used by geom and space
    void *data;		// user-defined data pointer
    dBodyID body;		// dynamics body associated with this object (if any)
    dxGeom *body_next;	// next geom in body's linked list of associated geoms
    dxPosR *final_posr;	// final position of the geom in world coordinates

    // information used by spaces
    dxGeom *next;		// next geom in linked list of geoms
    dxGeom **tome;	// linked list backpointer
    dxGeom *next_ex;	// next geom in extra linked list of geoms (for higher level structures)
    dxGeom **tome_ex;	// extra linked list backpointer (for higher level structures)
    dxSpace *parent_space;// the space this geom is contained in, 0 if none
    dReal aabb[6];	// cached AABB for this space
    unsigned long category_bits,collide_bits;

    dxGeom (dSpaceID _space, int is_placeable);
    virtual ~dxGeom();

    // Set or clear GEOM_ZERO_SIZED flag
    void updateZeroSizedFlag(bool is_zero_sized) { gflags = is_zero_sized ? (gflags | GEOM_ZERO_SIZED) : (gflags & ~GEOM_ZERO_SIZED); }
    // Get parent space TLS kind
    unsigned getParentSpaceTLSKind() const;

    const dVector3 &buildUpdatedPosition()
    {
        dIASSERT(gflags & GEOM_PLACEABLE);
        
        recomputePosr();
        return final_posr->pos;
    }

    const dMatrix3 &buildUpdatedRotation()
    {
        dIASSERT(gflags & GEOM_PLACEABLE);

        recomputePosr();
        return final_posr->R;
    }

    // recalculate our new final position if needed
    void recomputePosr()
    {
        if (gflags & GEOM_POSR_BAD) {
            computePosr();
            gflags &= ~GEOM_POSR_BAD;
        }
    }

    // calculate our new final position from our offset and body
    void computePosr();

    bool checkControlValueSizeValidity(void *dataValue, int *dataSize, int iRequiresSize) { return (*dataSize == iRequiresSize && dataValue != 0) ? true : !(*dataSize = iRequiresSize); } // Here it is the intent to return true for 0 required size in any case
    virtual bool controlGeometry(int controlClass, int controlCode, void *dataValue, int *dataSize);

    virtual void computeAABB()=0;
    // compute the AABB for this object and put it in aabb. this function
    // always performs a fresh computation, it does not inspect the
    // GEOM_AABB_BAD flag.

    virtual int AABBTest (dxGeom *o, dReal aabb[6]);
    // test whether the given AABB object intersects with this object, return
    // 1=yes, 0=no. this is used as an early-exit test in the space collision
    // functions. the default implementation returns 1, which is the correct
    // behavior if no more detailed implementation can be provided.

    // utility functions

    // compute the AABB only if it is not current. this function manipulates
    // the GEOM_AABB_BAD flag.

    void recomputeAABB() {
        if (gflags & GEOM_AABB_BAD) {
            // our aabb functions assume final_posr is up to date
            recomputePosr(); 
            computeAABB();
            gflags &= ~GEOM_AABB_BAD;
        }
    }

    inline void markAABBBad();

    // add and remove this geom from a linked list maintained by a space.

    void spaceAdd (dxGeom **first_ptr) {
        next = *first_ptr;
        tome = first_ptr;
        if (*first_ptr) (*first_ptr)->tome = &next;
        *first_ptr = this;
    }
    void spaceRemove() {
        if (next) next->tome = tome;
        *tome = next;
    }

    // add and remove this geom from a linked list maintained by a body.

    void bodyAdd (dxBody *b) {
        body = b;
        body_next = b->geom;
        b->geom = this;
    }
    void bodyRemove();
};

//****************************************************************************
// the base space class
//
// the contained geoms are divided into two kinds: clean and dirty.
// the clean geoms have not moved since they were put in the list,
// and their AABBs are valid. the dirty geoms have changed position, and
// their AABBs are may not be valid. the two types are distinguished by the
// GEOM_DIRTY flag. all dirty geoms come *before* all clean geoms in the list.

#if dTLS_ENABLED
#define dSPACE_TLS_KIND_INIT_VALUE OTK__DEFAULT
#define dSPACE_TLS_KIND_MANUAL_VALUE OTK_MANUALCLEANUP
#else
#define dSPACE_TLS_KIND_INIT_VALUE 0
#define dSPACE_TLS_KIND_MANUAL_VALUE 0
#endif

struct dxSpace : public dxGeom {
    int count;			// number of geoms in this space
    dxGeom *first;		// first geom in list
    int cleanup;			// cleanup mode, 1=destroy geoms on exit
    int sublevel;         // space sublevel (used in dSpaceCollide2). NOT TRACKED AUTOMATICALLY!!!
    unsigned tls_kind;	// space TLS kind to be used for global caches retrieval

    // cached state for getGeom()
    int current_index;		// only valid if current_geom != 0
    dxGeom *current_geom;		// if 0 then there is no information

    // locking stuff. the space is locked when it is currently traversing its
    // internal data structures, e.g. in collide() and collide2(). operations
    // that modify the contents of the space are not permitted when the space
    // is locked.
    int lock_count;

    dxSpace (dSpaceID _space);
    ~dxSpace();

    void computeAABB();

    void setCleanup (int mode) { cleanup = (mode != 0); }
    int getCleanup() const { return cleanup; }
    void setSublevel(int value) { sublevel = value; }
    int getSublevel() const { return sublevel; }
    void setManulCleanup(int value) { tls_kind = (value ? dSPACE_TLS_KIND_MANUAL_VALUE : dSPACE_TLS_KIND_INIT_VALUE); }
    int getManualCleanup() const { return (tls_kind == dSPACE_TLS_KIND_MANUAL_VALUE) ? 1 : 0; }
    int query (dxGeom *geom) const { dAASSERT(geom); return (geom->parent_space == this); }
    int getNumGeoms() const { return count; }

    virtual dxGeom *getGeom (int i);

    virtual void add (dxGeom *);
    virtual void remove (dxGeom *);
    virtual void dirty (dxGeom *);

    virtual void cleanGeoms()=0;
    // turn all dirty geoms into clean geoms by computing their AABBs and any
    // other space data structures that are required. this should clear the
    // GEOM_DIRTY and GEOM_AABB_BAD flags of all geoms.

    virtual void collide (void *data, dNearCallback *callback)=0;
    virtual void collide2 (void *data, dxGeom *geom, dNearCallback *callback)=0;
};


//////////////////////////////////////////////////////////////////////////

/*inline */
void dxGeom::markAABBBad() {
    gflags |= (GEOM_DIRTY | GEOM_AABB_BAD);
    CHECK_NOT_LOCKED(parent_space);
}


//****************************************************************************
// Initialization and finalization functions

void dInitColliders();
void dFinitColliders();

void dClearPosrCache(void);
void dFinitUserClasses();


#endif
