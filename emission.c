#include "changeset.h"
#include "database.h"
#include "emission.h"
#include "file.h"

#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>


void changeset_release (database_t * db, changeset_t * cs)
{
    assert (cs->unready_count != 0);

    if (--cs->unready_count == 0)
        heap_insert (&db->ready_changesets, cs);
}

void version_release (database_t * db, heap_t * version_heap,
                      version_t * version)
{
    if (version_heap)
        heap_insert (version_heap, version);

    changeset_release (db, version->commit);
}


void changeset_emitted (database_t * db, heap_t * ready_versions,
                        changeset_t * changeset)
{
    /* FIXME - this could just as well be merged into next_changeset. */

    if (changeset->type == ct_commit)
        for (version_t * i = changeset->versions; i; i = i->cs_sibling) {
            if (ready_versions)
                heap_remove (ready_versions, i);
            for (version_t * v = i->children; v; v = v->sibling)
                version_release (db, ready_versions, v);
        }

    for (changeset_t ** i = changeset->children;
         i != changeset->children_end; ++i)
        changeset_release (db, *i);
}


size_t changeset_update_branch_versions (struct database * db,
                                         struct changeset * changeset)
{
    version_t ** branch;
    bool implicit_merge = false;
    version_t * versions = changeset->versions;
    if (changeset->type == ct_implicit_merge) {
        assert (db->tags[0].tag[0] == 0);
        branch = db->tags[0].branch_versions;
        assert (branch);
        implicit_merge = true;
        versions = changeset->parent->versions;
    }
    else if (changeset->versions->branch == NULL)
        // FIXME - what should we do about changesets on anonymous branches?
        // Stringing them together into branches is probably more bother
        // than it's worth, so we should probably really just never actually
        // create those changesets.
        return 0;                   // Changeset on unknown branch.
    else
        branch = changeset->versions->branch->tag->branch_versions;

    size_t changes = 0;
    for (version_t * i = versions; i; i = i->cs_sibling) {
        if (implicit_merge && !i->implicit_merge)
            continue;
        version_t * v = i->dead ? NULL : i;
        if (branch[i->file - db->files] != v) {
            branch[i->file - db->files] = v;
            ++changes;
        }
    }

    return changes;
}


size_t changeset_update_branch_hash (struct database * db,
                                     struct changeset * changeset)
{
    size_t changes = changeset_update_branch_versions (db, changeset);
    if (changes == 0)
        return 0;

    version_t ** branch;
    if (changeset->type == ct_commit)
        branch = changeset->versions->branch->tag->branch_versions;
    else if (changeset->type == ct_implicit_merge)
        branch = db->tags[0].branch_versions;
    else
        abort();        

    // Compute the SHA1 hash of the current branch state.
    SHA_CTX sha;
    SHA1_Init (&sha);
    version_t ** branch_end = branch + (db->files_end - db->files);
    for (version_t ** i = branch; i != branch_end; ++i)
        if (*i != NULL && !(*i)->dead)
            SHA1_Update (&sha, i, sizeof (version_t *));

    uint32_t hash[5];
    SHA1_Final ((unsigned char *) hash, &sha);

    // Iterate over all the tags that match.  FIXME the duplicate flag is no
    // longer accurate.
    for (tag_t * i = database_tag_hash_find (db, hash); i;
         i = database_tag_hash_next (i)) {
        fprintf (stderr, "*** HIT %s %s%s ***\n",
                 i->branch_versions ? "BRANCH" : "TAG", i->tag,
                 i->changeset.parent
                 ? i->exact_match ? " (DUPLICATE)" : " (ALREADY EMITTED)" : "");
        if (i->changeset.parent == NULL) {
            // FIXME - we want better logic for exact matches following a
            // generic release.  Ideally an exact match would replace a generic
            // release if this does not risk introducing cycles.
            i->exact_match = true;
            changeset_add_child (changeset, &i->changeset);
        }
        if (!i->is_released) {
            i->is_released = true;
            heap_insert (&db->ready_tags, i);
        }
    }

    return changes;
}


static const version_t * preceed (const version_t * v)
{
    // If cs is not ready to emit, then some version in cs is blocked.  The
    // earliest un-emitted ancestor of that version will be ready to emit.
    // Search for it.  FIXME We should be a bit smarter by searching harder for
    // the oldest possible version.
    for (version_t * csv = v->commit->versions; csv; csv = csv->cs_sibling)
        if (csv->ready_index == SIZE_MAX)
            for (version_t * v = csv->parent; v; v = v->parent)
                if (v->ready_index != SIZE_MAX)
                    return v;

    abort();
}


static void cycle_split (database_t * db, changeset_t * cs)
{
    // FIXME - the changeset may have an implicit merge; we should then split
    // the implicit merge also.
    fflush (NULL);
    fprintf (stderr, "*********** CYCLE **********\n");
    // We split the changeset into to.  We leave all the blocked versions
    // in cs, and put the ready-to-emit into nw.

    // FIXME - we should split implicit merges also.
    changeset_t * new = database_new_changeset (db);
    new->type = ct_commit;
    new->time = cs->time;
    version_t ** cs_v = &cs->versions;
    version_t ** new_v = &new->versions;
    for (version_t * v = cs->versions; v; v = v->cs_sibling) {
        assert (!v->implicit_merge);    // Not yet handled.
        if (v->ready_index == SIZE_MAX) {
            // Blocked; stays in cs.
            *cs_v = v;
            cs_v = &v->cs_sibling;
        }
        else {
            // Ready-to-emit; goes into new.
            v->commit = new;
            *new_v = v;
            new_v = &v->cs_sibling;
        }
    }

    *cs_v = NULL;
    *new_v = NULL;
    assert (cs->versions);
    assert (new->versions);

    heap_insert (&db->ready_changesets, new);

    fprintf (stderr, "Changeset %s %s\n%s\n",
             cs->versions->branch ? cs->versions->branch->tag->tag : "",
             cs->versions->author, cs->versions->log);
    for (const version_t * v = new->versions; v; v = v->cs_sibling)
        fprintf (stderr, "    %s:%s\n", v->file->rcs_path, v->version);

    fprintf (stderr, "Deferring:\n");

    for (const version_t * v = cs->versions; v; v = v->cs_sibling)
        fprintf (stderr, "    %s:%s\n", v->file->rcs_path, v->version);
}


static const version_t * cycle_find (const version_t * v)
{
    const version_t * slow = v;
    const version_t * fast = v;
    do {
        slow = preceed (slow);
        fast = preceed (preceed (fast));
    }
    while (slow != fast);
    return slow;
}


changeset_t * next_changeset_split (database_t * db, heap_t * ready_versions)
{
    if (heap_empty (ready_versions))
        return NULL;

    if (heap_empty (&db->ready_changesets)) {
        // Find a cycle.
        const version_t * slow = heap_front (ready_versions);
        const version_t * fast = slow;
        do {
            slow = preceed (slow);
            fast = preceed (preceed (fast));
        }
        while (slow != fast);

        // And split it.
        cycle_split (db, cycle_find (heap_front (ready_versions))->commit);

        assert (db->ready_changesets.entries
                != db->ready_changesets.entries_end);
    }

    return heap_pop (&db->ready_changesets);
}


changeset_t * next_changeset (database_t * db)
{
    if (heap_empty (&db->ready_changesets))
        return NULL;
    else
        return heap_pop (&db->ready_changesets);
}


void prepare_for_emission (database_t * db, heap_t * ready_versions)
{
    // Re-do the changeset unready counts.
    for (changeset_t ** i = db->changesets; i != db->changesets_end; ++i) {
        if ((*i)->type == ct_commit)
            for (version_t * j = (*i)->versions; j; j = j->cs_sibling)
                ++(*i)->unready_count;

        for (changeset_t ** j = (*i)->children; j != (*i)->children_end; ++j)
            ++(*j)->unready_count;
    }

    // Mark the initial versions as ready to emit.
    for (file_t * f = db->files; f != db->files_end; ++f)
        for (version_t * j = f->versions; j != f->versions_end; ++j)
            if (j->parent == NULL)
                version_release (db, ready_versions, j);
}
