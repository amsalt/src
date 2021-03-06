/* $NetBSD: drm_gem_cma_helper.c,v 1.1 2017/12/26 14:53:12 jmcneill Exp $ */

/*-
 * Copyright (c) 2015-2017 Jared McNeill <jmcneill@invisible.ca>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: drm_gem_cma_helper.c,v 1.1 2017/12/26 14:53:12 jmcneill Exp $");

#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>

#include <uvm/uvm.h>

struct drm_gem_cma_object *
drm_gem_cma_create(struct drm_device *ddev, unsigned int size)
{
	struct drm_gem_cma_object *obj;
	int error, nsegs;

	obj = kmem_zalloc(sizeof(*obj), KM_SLEEP);
	obj->dmat = ddev->bus_dmat;
	obj->dmasize = size;

        error = bus_dmamem_alloc(obj->dmat, obj->dmasize, PAGE_SIZE, 0,
	    obj->dmasegs, 1, &nsegs, BUS_DMA_WAITOK);
	if (error)
		goto failed;
	error = bus_dmamem_map(obj->dmat, obj->dmasegs, nsegs,
	    obj->dmasize, &obj->vaddr, BUS_DMA_WAITOK | BUS_DMA_COHERENT);
	if (error)
		goto free;
	error = bus_dmamap_create(obj->dmat, obj->dmasize, 1,
	    obj->dmasize, 0, BUS_DMA_WAITOK, &obj->dmamap);
	if (error)
		goto unmap;
	error = bus_dmamap_load(obj->dmat, obj->dmamap, obj->vaddr,
	    obj->dmasize, NULL, BUS_DMA_WAITOK);
	if (error)
		goto destroy;

	memset(obj->vaddr, 0, obj->dmasize);

	drm_gem_private_object_init(ddev, &obj->base, size);

	return obj;

destroy:
	bus_dmamap_destroy(obj->dmat, obj->dmamap);
unmap:
	bus_dmamem_unmap(obj->dmat, obj->vaddr, obj->dmasize);
free:
	bus_dmamem_free(obj->dmat, obj->dmasegs, nsegs);
failed:
	kmem_free(obj, sizeof(*obj));

	return NULL;
}

static void
drm_gem_cma_obj_free(struct drm_gem_cma_object *obj)
{
	bus_dmamap_unload(obj->dmat, obj->dmamap);
	bus_dmamap_destroy(obj->dmat, obj->dmamap);
	bus_dmamem_unmap(obj->dmat, obj->vaddr, obj->dmasize);
	bus_dmamem_free(obj->dmat, obj->dmasegs, 1);
	kmem_free(obj, sizeof(*obj));
}

void
drm_gem_cma_free_object(struct drm_gem_object *gem_obj)
{
	struct drm_gem_cma_object *obj = to_drm_gem_cma_obj(gem_obj);

	drm_gem_free_mmap_offset(gem_obj);
	drm_gem_object_release(gem_obj);
	drm_gem_cma_obj_free(obj);
}

int
drm_gem_cma_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
    struct drm_mode_create_dumb *args)
{
	struct drm_gem_cma_object *obj;
	uint32_t handle;
	int error;

	args->pitch = args->width * ((args->bpp + 7) / 8);
	args->size = args->pitch * args->height;
	args->size = roundup(args->size, PAGE_SIZE);
	args->handle = 0;

	obj = drm_gem_cma_create(ddev, args->size);
	if (obj == NULL)
		return -ENOMEM;

	error = drm_gem_handle_create(file_priv, &obj->base, &handle);
	drm_gem_object_unreference_unlocked(&obj->base);
	if (error) {
		drm_gem_cma_obj_free(obj);
		return error;
	}

	args->handle = handle;

	return 0;
}

int
drm_gem_cma_dumb_map_offset(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	struct drm_gem_cma_object *obj;
	int error;

	gem_obj = drm_gem_object_lookup(ddev, file_priv, handle);
	if (gem_obj == NULL)
		return -ENOENT;

	obj = to_drm_gem_cma_obj(gem_obj);

	if (drm_vma_node_has_offset(&obj->base.vma_node) == 0) {
		error = drm_gem_create_mmap_offset(&obj->base);
		if (error)
			goto done;
	} else {
		error = 0;
	}

	*offset = drm_vma_node_offset_addr(&obj->base.vma_node);

done:
	drm_gem_object_unreference_unlocked(&obj->base);

	return error;
}

static int
drm_gem_cma_fault(struct uvm_faultinfo *ufi, vaddr_t vaddr,
    struct vm_page **pps, int npages, int centeridx, vm_prot_t access_type,
    int flags)
{
	struct vm_map_entry *entry = ufi->entry;
	struct uvm_object *uobj = entry->object.uvm_obj;
	struct drm_gem_object *gem_obj =
	    container_of(uobj, struct drm_gem_object, gemo_uvmobj);
	struct drm_gem_cma_object *obj = to_drm_gem_cma_obj(gem_obj);
	off_t curr_offset;
	vaddr_t curr_va;
	paddr_t paddr, mdpgno;
	u_int mmapflags;
	int lcv, retval;
	vm_prot_t mapprot;

	if (UVM_ET_ISCOPYONWRITE(entry))
		return -EIO;

	curr_offset = entry->offset + (vaddr - entry->start);
	curr_va = vaddr;

	retval = 0;
	for (lcv = 0; lcv < npages; lcv++, curr_offset += PAGE_SIZE,
	    curr_va += PAGE_SIZE) {
		if ((flags & PGO_ALLPAGES) == 0 && lcv != centeridx)
			continue;
		if (pps[lcv] == PGO_DONTCARE)
			continue;

		mdpgno = bus_dmamem_mmap(obj->dmat, obj->dmasegs, 1,
		    curr_offset, access_type, BUS_DMA_PREFETCHABLE);
		if (mdpgno == -1) {
			retval = -EIO;
			break;
		}
		paddr = pmap_phys_address(mdpgno);
		mmapflags = pmap_mmap_flags(mdpgno);
		mapprot = ufi->entry->protection;

		if (pmap_enter(ufi->orig_map->pmap, curr_va, paddr, mapprot,
		    PMAP_CANFAIL | mapprot | mmapflags) != 0) {
			pmap_update(ufi->orig_map->pmap);
			uvmfault_unlockall(ufi, ufi->entry->aref.ar_amap, uobj);
			uvm_wait("drm_gem_cma_fault");
			return -ERESTART;
		}
	}

	pmap_update(ufi->orig_map->pmap);
	uvmfault_unlockall(ufi, ufi->entry->aref.ar_amap, uobj);

	return retval;
}

const struct uvm_pagerops drm_gem_cma_uvm_ops = {
	.pgo_reference = drm_gem_pager_reference,
	.pgo_detach = drm_gem_pager_detach,
	.pgo_fault = drm_gem_cma_fault,
};
