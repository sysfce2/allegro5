/*         ______   ___    ___ 
 *        /\  _  \ /\_ \  /\_ \ 
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___ 
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      Event queues.
 *
 *      By Peter Wang.
 *
 *      See readme.txt for copyright information.
 */

/* Title: Event queues
 *
 * An event queue buffers events generated by event sources that were
 * registered with the queue.
 */


#include <string.h>

#include "allegro5/allegro5.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_dtor.h"
#include "allegro5/internal/aintern_memory.h"
#include "allegro5/internal/aintern_events.h"



struct ALLEGRO_EVENT_QUEUE
{
   _AL_VECTOR sources;  /* vector of (ALLEGRO_EVENT_SOURCE *) */
   _AL_VECTOR events;   /* vector of ALLEGRO_EVENT, used as circular array */
   unsigned int events_head;  /* write end of circular array */
   unsigned int events_tail;  /* read end of circular array */
   _AL_MUTEX mutex;
   _AL_COND cond;
};



/* forward declarations */
static bool do_wait_for_event(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT *ret_event, ALLEGRO_TIMEOUT *timeout);
static void copy_event(ALLEGRO_EVENT *dest, const ALLEGRO_EVENT *src);
static void discard_events_of_source(ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT_SOURCE *source);



/* Function: al_create_event_queue
 *  Create a new, empty event queue, returning a pointer to object if
 *  successful.  Returns NULL on error.
 */
ALLEGRO_EVENT_QUEUE *al_create_event_queue(void)
{
   ALLEGRO_EVENT_QUEUE *queue = _AL_MALLOC(sizeof *queue);
   int i;

   ASSERT(queue);

   if (queue) {
      _al_vector_init(&queue->sources, sizeof(ALLEGRO_EVENT_SOURCE *));

      _al_vector_init(&queue->events, sizeof(ALLEGRO_EVENT));
      _al_vector_alloc_back(&queue->events);
      queue->events_head = 0;
      queue->events_tail = 0;

      _AL_MARK_MUTEX_UNINITED(queue->mutex);
      _al_mutex_init(&queue->mutex);
      _al_cond_init(&queue->cond);

      _al_register_destructor(queue, (void (*)(void *)) al_destroy_event_queue);
   }

   return queue;
}



/* Function: al_destroy_event_queue
 *  Destroy the event queue specified.  All event sources currently
 *  registered with the queue will be automatically unregistered before
 *  the queue is destroyed.
 */
void al_destroy_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   ALLEGRO_EVENT *event;
   ASSERT(queue);

   _al_unregister_destructor(queue);

   /* Unregister any event sources registered with this queue.  */
   while (_al_vector_is_nonempty(&queue->sources)) {
      ALLEGRO_EVENT_SOURCE **slot = _al_vector_ref_back(&queue->sources);
      al_unregister_event_source(queue, *slot);
   }

   ASSERT(_al_vector_is_empty(&queue->sources));
   _al_vector_free(&queue->sources);

   ASSERT(queue->events_head == queue->events_tail);
   _al_vector_free(&queue->events);

   _al_cond_destroy(&queue->cond);
   _al_mutex_destroy(&queue->mutex);

   _AL_FREE(queue);
}



/* Function: al_register_event_source
 *  Register the event source with the event queue specified.  An
 *  event source may be registered with any number of event queues
 *  simultaneously, or none.  Trying to register an event source with
 *  the same event queue more than once does nothing.
 */
void al_register_event_source(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT_SOURCE *source)
{
   ALLEGRO_EVENT_SOURCE **slot;
   ASSERT(queue);
   ASSERT(source);

   if (!_al_vector_contains(&queue->sources, &source)) {
      _al_event_source_on_registration_to_queue(source, queue);
      _al_mutex_lock(&queue->mutex);
      slot = _al_vector_alloc_back(&queue->sources);
      *slot = source;
      _al_mutex_unlock(&queue->mutex);
   }
}



/* Function: al_unregister_event_source
 *  Unregister an event source with an event queue.  If the event
 *  source is not actually registered with the event queue, nothing
 *  happens.
 *
 *  If the queue had any events in it which originated from the event
 *  source, they will no longer be in the queue after this call.
 */
void al_unregister_event_source(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT_SOURCE *source)
{
   bool found;
   ASSERT(queue);
   ASSERT(source);

   /* Remove source from our list. */
   _al_mutex_lock(&queue->mutex);
   found = _al_vector_find_and_delete(&queue->sources, &source);
   _al_mutex_unlock(&queue->mutex);

   if (found) {
      /* Tell the event source that it was unregistered. */
      _al_event_source_on_unregistration_from_queue(source, queue);

      /* Drop all the events in the queue that belonged to the source. */
      _al_mutex_lock(&queue->mutex);
      discard_events_of_source(queue, source);
      _al_mutex_unlock(&queue->mutex);
   }
}



/* Function: al_event_queue_is_empty
 *  Return true if the event queue specified is currently empty.
 */
bool al_event_queue_is_empty(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   return (queue->events_head == queue->events_tail);
}



/* circ_array_next:
 *  Return the next index in a circular array.
 */
static int circ_array_next(const _AL_VECTOR *vector, int i)
{
   return (i + 1) % _al_vector_size(vector);
}



/* get_next_event_if_any: [primary thread]
 *  Helper function.  It returns a pointer to the next event in the
 *  queue, or NULL.  Optionally the event is removed from the queue.
 *  However, the event is _not released_ (which is the caller's
 *  responsibility).  The event queue must be locked before entering
 *  this function.
 */
static ALLEGRO_EVENT *get_next_event_if_any(ALLEGRO_EVENT_QUEUE *queue,
   bool delete)
{
   ALLEGRO_EVENT *event;

   if (al_event_queue_is_empty(queue)) {
      return NULL;
   }

   event = _al_vector_ref(&queue->events, queue->events_tail);
   if (delete) {
      queue->events_tail = circ_array_next(&queue->events, queue->events_tail);
   }
   return event;
}



/* get_peek_or_drop_next_event: [primary thread]
 *  Helper function to do the work for al_get_next_event,
 *  al_peek_next_event and al_drop_next_event which are all very
 *  similar.
 */
static bool get_peek_or_drop_next_event(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT *do_copy, bool delete)
{
   ALLEGRO_EVENT *next_event;

   _al_mutex_lock(&queue->mutex);

   next_event = get_next_event_if_any(queue, delete);
   if (next_event) {
      if (do_copy)
         copy_event(do_copy, next_event);
   }

   _al_mutex_unlock(&queue->mutex);

   return (next_event ? true : false);
}



/* Function: al_get_next_event
 *  Take the next event packet out of the event queue specified, and
 *  copy the contents into RET_EVENT, returning true.  The original
 *  event packet will be removed from the queue.  If the event queue is
 *  empty, return false and the contents of RET_EVENT are unspecified.
 */
bool al_get_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ASSERT(queue);
   ASSERT(ret_event);

   return get_peek_or_drop_next_event(queue, ret_event, true);
}



/* Function: al_peek_next_event
 *  Copy the contents of the next event packet in the event queue
 *  specified into RET_EVENT and return true.  The original event
 *  packet will remain at the head of the queue.  If the event queue is
 *  actually empty, this function returns false and the contents of
 *  RET_EVENT are unspecified.
 */
bool al_peek_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ASSERT(queue);
   ASSERT(ret_event);

   return get_peek_or_drop_next_event(queue, ret_event, false);
}



/* Function: al_drop_next_event
 *  Drop the next event packet from the queue.  If the queue is empty,
 *  nothing happens.
 */
void al_drop_next_event(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   get_peek_or_drop_next_event(queue, NULL, true);
}



/* Function: al_flush_event_queue
 *  Drops all events, if any, from the queue.
 */
void al_flush_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   _al_mutex_lock(&queue->mutex);
   queue->events_head = queue->events_tail = 0;
   _al_mutex_unlock(&queue->mutex);
}



/* [primary thread]
 *
 * Function: al_wait_for_event
 *  Wait until the event queue specified is non-empty.  If RET_EVENT
 *  is not NULL, the first event packet in the queue will be copied
 *  into RET_EVENT and removed from the queue.  If RET_EVENT is NULL
 *  the first event packet is left at the head of the queue.
 */
void al_wait_for_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ALLEGRO_EVENT *next_event = NULL;

   ASSERT(queue);

   _al_mutex_lock(&queue->mutex);
   {
      while (al_event_queue_is_empty(queue)) {
         _al_cond_wait(&queue->cond, &queue->mutex);
      }

      if (ret_event) {
         next_event = get_next_event_if_any(queue, true);
         copy_event(ret_event, next_event);
      }
   }
   _al_mutex_unlock(&queue->mutex);
}



/* [primary thread]
 *
 * Function: al_wait_for_event_timed
 *  Wait until the event queue specified is non-empty.  If RET_EVENT
 *  is not NULL, the first event packet in the queue will be copied
 *  into RET_EVENT and removed from the queue.  If RET_EVENT is NULL
 *  the first event packet is left at the head of the queue.
 *
 *  TIMEOUT_MSECS determines approximately how many seconds to
 *  wait.  If the call times out, false is returned.  Otherwise true is
 *  returned.
 */
bool al_wait_for_event_timed(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT *ret_event, float secs)
{
   ALLEGRO_TIMEOUT timeout;

   ASSERT(queue);
   ASSERT(secs >= 0);

   if (secs < 0.0)
      al_init_timeout(&timeout, 0);
   else
      al_init_timeout(&timeout, secs);

   return do_wait_for_event(queue, ret_event, &timeout);
}



bool al_wait_for_event_until(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT *ret_event, ALLEGRO_TIMEOUT *timeout)
{
   ASSERT(queue);

   return do_wait_for_event(queue, ret_event, timeout);
}



static bool do_wait_for_event(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT *ret_event, ALLEGRO_TIMEOUT *timeout)
{
   bool timed_out = false;
   ALLEGRO_EVENT *next_event = NULL;

   _al_mutex_lock(&queue->mutex);
   {
      int result = 0;

      /* Is the queue is non-empty?  If not, block on a condition
       * variable, which will be signaled when an event is placed into
       * the queue.
       */
      while (al_event_queue_is_empty(queue) && (result != -1)) {
         result = _al_cond_timedwait(&queue->cond, &queue->mutex, timeout);
      }

      if (result == -1)
         timed_out = true;
      else if (ret_event) {
         next_event = get_next_event_if_any(queue, true);
         copy_event(ret_event, next_event);
      }
   }
   _al_mutex_unlock(&queue->mutex);

   if (timed_out)
      return false;

   return true;
}



/* expand_events_array:
 *  Expand the circular array holding events.
 */
static void expand_events_array(ALLEGRO_EVENT_QUEUE *queue)
{
   /* The underlying vector grows by powers of two. */
   const size_t old_size = _al_vector_size(&queue->events);
   const size_t new_size = old_size * 2;
   unsigned int i;

   for (i = old_size; i < new_size; i++) {
      _al_vector_alloc_back(&queue->events);
   }

   /* Move wrapped-around elements at the start of the array to the back. */
   if (queue->events_head < queue->events_tail) {
      for (i = 0; i < queue->events_head; i++) {
         ALLEGRO_EVENT *old_ev = _al_vector_ref(&queue->events, i);
         ALLEGRO_EVENT *new_ev = _al_vector_ref(&queue->events, old_size + i);
         copy_event(new_ev, old_ev);
      }
      queue->events_head += old_size;
   }
}


/* alloc_event:
 *
 *  The event source must be _locked_ before calling this function.
 *
 *  [runs in background threads]
 */
static ALLEGRO_EVENT *alloc_event(ALLEGRO_EVENT_QUEUE *queue)
{
   ALLEGRO_EVENT *event;
   unsigned int adv_head;

   adv_head = circ_array_next(&queue->events, queue->events_head);
   if (adv_head == queue->events_tail) {
      expand_events_array(queue);
      adv_head = circ_array_next(&queue->events, queue->events_head);
   }

   event = _al_vector_ref(&queue->events, queue->events_head);
   queue->events_head = adv_head;
   return event;
}



/* copy_event:
 *  Copies the contents of the event SRC to DEST.
 */
static void copy_event(ALLEGRO_EVENT *dest, const ALLEGRO_EVENT *src)
{
   ASSERT(dest);
   ASSERT(src);

   *dest = *src;
}



/* Internal function: _al_event_queue_push_event
 *  Event sources call this function when they have something to add to
 *  the queue.  If a queue cannot accept the event, the event's
 *  refcount will not be incremented.
 *
 *  If no event queues can accept the event, the event should be
 *  returned to the event source's list of recyclable events.
 */
void _al_event_queue_push_event(ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT *orig_event)
{
   ALLEGRO_EVENT *new_event;
   ASSERT(queue);
   ASSERT(orig_event);

   _al_mutex_lock(&queue->mutex);
   {
      new_event = alloc_event(queue);
      copy_event(new_event, orig_event);

      /* Wake up threads that are waiting for an event to be placed in
       * the queue.
       */
      _al_cond_broadcast(&queue->cond);
   }
   _al_mutex_unlock(&queue->mutex);
}



/* contains_event_of_source:
 *  Return true iff the event queue contains an event from the given source.
 *  The queue must be locked.
 */
static bool contains_event_of_source(const ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT_SOURCE *source)
{
   ALLEGRO_EVENT *event;
   unsigned int i;

   i = queue->events_tail;
   while (i != queue->events_head) {
      event = _al_vector_ref(&queue->events, i);
      if (event->any.source == source) {
         return true;
      }
      i = circ_array_next(&queue->events, i);
   }

   return false;
}



/* Helper to get smallest fitting power of two. */
static int pot(int x)
{
   int y = 1;
   while (y < x) y *= 2;
   return y;
}



/* discard_events_of_source:
 *  Discard all the events in the queue that belong to the source.
 *  The queue must be locked.
 */
static void discard_events_of_source(ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT_SOURCE *source)
{
   _AL_VECTOR old_events;
   ALLEGRO_EVENT *old_event;
   ALLEGRO_EVENT *new_event;
   size_t old_size;
   size_t new_size;
   unsigned int i;

   if (!contains_event_of_source(queue, source)) {
      return;
   }

   /* Copy elements we want to keep from the old vector to a new one. */
   old_events = queue->events;
   _al_vector_init(&queue->events, sizeof(ALLEGRO_EVENT));

   i = queue->events_tail;
   while (i != queue->events_head) {
      old_event = _al_vector_ref(&old_events, i);
      if (old_event->any.source != source) {
         new_event = _al_vector_alloc_back(&queue->events);
         copy_event(new_event, old_event);
      }
      i = circ_array_next(&old_events, i);
   }

   queue->events_tail = 0;
   queue->events_head = _al_vector_size(&queue->events);

   /* The circular array always needs at least one unused element. */
   old_size = _al_vector_size(&queue->events);
   new_size = pot(old_size + 1);
   for (i = old_size; i < new_size; i++) {
      _al_vector_alloc_back(&queue->events);
   }

   _al_vector_free(&old_events);
}



/*
 * Local Variables:
 * c-basic-offset: 3
 * indent-tabs-mode: nil
 * End:
 */
/* vim: set sts=3 sw=3 et: */
