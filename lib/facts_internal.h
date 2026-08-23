#ifndef FACTS_INTERNAL_H
#define FACTS_INTERNAL_H

#include "facts.h"

/* Reserved for storage-engine integrations such as Linda coordination. */
void facts_register_internal_tx_listener(s_facts *facts, f_facts_tx_listener listener, void *user_data);
void facts_register_internal_commit_summary_observer(s_facts *facts, f_facts_commit_summary_observer observer, void *user_data);
int facts_justifications_stage(s_facts *facts, int preserve_existing);
void facts_justifications_commit(s_facts *facts);
void facts_justifications_discard(s_facts *facts);
int facts_reset_local_db(s_facts *facts);

#endif
