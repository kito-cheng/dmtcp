/* NOTE:  if you just want to insert your own code at the time of checkpoint
 *  and restart, there are two simpler additional mechanisms:
 *  dmtcpaware, and the MTCP special hook functions:
 *    mtcpHookPreCheckpoint, mtcpHookPostCheckpoint, mtcpHookRestart
 */

#include <stdio.h>
#include "dmtcpplugin.h"


void dmtcp_process_event(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  /* NOTE:  See warning in plugin/README about calls to printf here. */
  switch (event) {
  case DMTCP_EVENT_INIT:
    printf("The plugin containing %s has been initialized.\n", __FILE__);
    break;
  case DMTCP_EVENT_PRE_CKPT:
    printf("\n*** The plugin is being called before checkpointing. ***\n");
    break;
  case DMTCP_EVENT_POST_CKPT:
    printf("*** The plugin has now been checkpointed. ***\n");
    break;
  case DMTCP_EVENT_RESUME:
    if (data->resumeInfo.isRestart) {
      printf("The plugin is now resuming or restarting from checkpointing.\n");
    } else {
      printf("The process is now resuming after checkpoint.\n");
    }
    break;
  case DMTCP_EVENT_PRE_EXIT:
    printf("The plugin is being called before exiting.\n");
    break;
  /* These events are unused and could be omitted.  See dmtcpplugin.h for
   * complete list.
   */
  case DMTCP_EVENT_POST_RESTART:
  case DMTCP_EVENT_RESET_ON_FORK:
  case DMTCP_EVENT_SUSPENDED:
  case DMTCP_EVENT_LEADER_ELECTION:
  case DMTCP_EVENT_DRAIN:
  default:
    break;
  }
  NEXT_DMTCP_PROCESS_EVENT(event, data);
}
