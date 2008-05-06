#ifndef EMISSION_H
#define EMISSION_H

struct changeset;
struct database;
struct heap;
struct version;

void changeset_release (struct database * db, struct changeset * cs);

/// Mark a version as ready to be emitted.
void version_release (struct database * db,
                      struct heap * ready_versions,
                      struct version * version);

/// Record that a changeset has been emitted; release child versions and
/// changesets.  @c ready_versions may be NULL if not in use.
void changeset_emitted (struct database * db, struct heap * ready_versions,
                        struct changeset * changeset);

/// Record the new changeset versions on the corresponding branch.  Return the
/// number of files that actually changed.  This may be zero if the changeset
/// consisted entirely of dead trunk 1.1 revisions corresponding to branch
/// additions.
size_t changeset_update_branch_versions (struct database * db,
                                         struct changeset * changeset);

/// Record the new changeset versions; update the branch hash and find any
/// matching tags.
size_t changeset_update_branch_hash (struct database * db,
                                     struct changeset * changeset);

/// Find the next changeset to emit; split cycles if necessary.
changeset_t * next_changeset_split (struct database * db,
                                    struct heap * ready_versions);

/// Find the next changeset to emit.
changeset_t * next_changeset (struct database * db);

/// Set up all the unready_counts, and mark initial versions as ready to emit.
void prepare_for_emission (struct database * db, struct heap * ready_versions);

#endif
