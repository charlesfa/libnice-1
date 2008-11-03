/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006, 2007 Collabora Ltd.
 *  Contact: Dafydd Harries
 * (C) 2006, 2007 Nokia Corporation. All rights reserved.
 *  Contact: Kai Vehmanen
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Dafydd Harries, Collabora Ltd.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "gstnicesrc.h"

GST_DEBUG_CATEGORY_STATIC (nicesrc_debug);
#define GST_CAT_DEFAULT nicesrc_debug


#define BUFFER_SIZE (65536)

static GstFlowReturn
gst_nice_src_create (
  GstBaseSrc *basesrc,
  guint64 offset,
  guint length,
  GstBuffer **buffer);

static gboolean
gst_nice_src_unlock (
    GstBaseSrc *basesrc);

static gboolean
gst_nice_src_unlock_stop (
    GstBaseSrc *basesrc);

static void
gst_nice_src_set_property (
  GObject *object,
  guint prop_id,
  const GValue *value,
  GParamSpec *pspec);

static void
gst_nice_src_get_property (
  GObject *object,
  guint prop_id,
  GValue *value,
  GParamSpec *pspec);


static void
gst_nice_src_dispose (GObject *object);

static GstStateChangeReturn
gst_nice_src_change_state (
    GstElement * element,
    GstStateChange transition);

static const GstElementDetails gst_nice_src_details =
GST_ELEMENT_DETAILS (
    "ICE source",
    "Source",
    "Interactive UDP connectivity establishment",
    "Dafydd Harries <dafydd.harries@collabora.co.uk>");

static GstStaticPadTemplate gst_nice_src_src_template =
GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

GST_BOILERPLATE (GstNiceSrc, gst_nice_src, GstBaseSrc, GST_TYPE_BASE_SRC);

enum
{
  PROP_AGENT = 1,
  PROP_STREAM,
  PROP_COMPONENT
};

static void
gst_nice_src_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_nice_src_src_template));
  gst_element_class_set_details (element_class, &gst_nice_src_details);
}

static void
gst_nice_src_class_init (GstNiceSrcClass *klass)
{
  GstBaseSrcClass *gstbasesrc_class;
  GstElementClass *gstelement_class;
  GObjectClass *gobject_class;

  GST_DEBUG_CATEGORY_INIT (nicesrc_debug, "nicesrc",
      0, "libnice source");

  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR (gst_nice_src_create);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_nice_src_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_nice_src_unlock_stop);

  gobject_class = (GObjectClass *) klass;
  gobject_class->set_property = gst_nice_src_set_property;
  gobject_class->get_property = gst_nice_src_get_property;
  gobject_class->dispose = gst_nice_src_dispose;

  gstelement_class = (GstElementClass *) klass;
  gstelement_class->change_state = gst_nice_src_change_state;

  g_object_class_install_property (gobject_class, PROP_AGENT,
      g_param_spec_object (
         "agent",
         "Agent",
         "The NiceAgent this source is bound to",
         NICE_TYPE_AGENT,
         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_STREAM,
      g_param_spec_uint (
         "stream",
         "Stream ID",
         "The ID of the stream to read from",
         0,
         G_MAXUINT,
         0,
         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_COMPONENT,
      g_param_spec_uint (
         "component",
         "Component ID",
         "The ID of the component to read from",
         0,
         G_MAXUINT,
         0,
         G_PARAM_READWRITE));
}

static void
gst_nice_src_init (GstNiceSrc *src, GstNiceSrcClass *g_class)
{
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (src), TRUE);
  src->agent = NULL;
  src->stream_id = 0;
  src->component_id = 0;
  src->mainloop = g_main_loop_new (g_main_context_new (), FALSE);
  src->unlocked = FALSE;
  src->idle_source = NULL;
}

static void
gst_nice_src_read_callback (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    guint len,
    gchar *buf,
    gpointer data)
{
  GstBaseSrc *basesrc = GST_BASE_SRC (data);
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);
  GstNetBuffer *mybuf;

  GST_LOG_OBJECT (agent, "Got buffer, getting out of the main loop");

  mybuf = gst_netbuffer_new ();
  GST_BUFFER_MALLOCDATA (mybuf) = g_memdup (buf, len);
  GST_BUFFER_SIZE (mybuf) = len;
  GST_BUFFER_DATA (mybuf) = GST_BUFFER_MALLOCDATA (mybuf);
  if (GST_PAD_CAPS (basesrc->srcpad))
    GST_BUFFER_CAPS (mybuf) = gst_caps_ref (GST_PAD_CAPS (basesrc->srcpad));

  mybuf->from = nicesrc->from;
  mybuf->to = nicesrc->to;

  nicesrc->outbuf = GST_BUFFER_CAST (mybuf);

  g_main_loop_quit (nicesrc->mainloop);
}

static gboolean
gst_nice_src_unlock_idler (gpointer data)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (data);

  g_main_loop_quit (nicesrc->mainloop);

  GST_OBJECT_LOCK (nicesrc);
  g_source_unref (nicesrc->idle_source);
  nicesrc->idle_source = NULL;
  GST_OBJECT_UNLOCK (nicesrc);

  return FALSE;
}

static gboolean
gst_nice_src_unlock (GstBaseSrc *src)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (src);

  GST_OBJECT_LOCK (src);
  nicesrc->unlocked = TRUE;

  g_main_loop_quit (nicesrc->mainloop);

  nicesrc->idle_source = g_idle_source_new ();
  g_source_set_priority (nicesrc->idle_source, G_PRIORITY_HIGH);
  g_source_set_callback (nicesrc->idle_source, gst_nice_src_unlock_idler, src, NULL);
  g_source_attach (nicesrc->idle_source, g_main_loop_get_context (nicesrc->mainloop));
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static gboolean
gst_nice_src_unlock_stop (GstBaseSrc *src)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (src);

  GST_OBJECT_LOCK (src);
  nicesrc->unlocked = FALSE;
  if (nicesrc->idle_source)
    g_source_destroy (nicesrc->idle_source);
  nicesrc->idle_source = NULL;
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static GstFlowReturn
gst_nice_src_create (
  GstBaseSrc *basesrc,
  guint64 offset,
  guint length,
  GstBuffer **buffer)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);

  GST_LOG_OBJECT (nicesrc, "create called");

  nicesrc->outbuf = NULL;

  GST_OBJECT_LOCK (basesrc);
  if (nicesrc->unlocked) {
    GST_OBJECT_UNLOCK (basesrc);
    return GST_FLOW_WRONG_STATE;
  }
  GST_OBJECT_UNLOCK (basesrc);

  g_main_loop_run (nicesrc->mainloop);

  if (nicesrc->outbuf) {
    GST_LOG_OBJECT (nicesrc, "Got buffer, pushing");

    *buffer = nicesrc->outbuf;
    GST_BUFFER_OFFSET (*buffer) = offset;

    return GST_FLOW_OK;
  } else {
    GST_LOG_OBJECT (nicesrc, "Got interrupting, returning wrong-state");
    return GST_FLOW_WRONG_STATE;
  }
}

static void
gst_nice_src_dispose (GObject *object)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  if (src->new_selected_pair_id)
    g_signal_handler_disconnect (src->agent, src->new_selected_pair_id);
  src->new_selected_pair_id = 0;

  if (src->agent)
    g_object_unref (src->agent);
  src->agent = NULL;

  if (src->mainloop)
    g_main_loop_unref (src->mainloop);
  src->mainloop = NULL;

  GST_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
gst_nice_src_set_property (
  GObject *object,
  guint prop_id,
  const GValue *value,
  GParamSpec *pspec)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  switch (prop_id)
    {
    case PROP_AGENT:
      if (src->agent)
        GST_ERROR_OBJECT (object,
            "Changing the agent on a nice src not allowed");
      else
        src->agent = g_value_dup_object (value);
      break;

    case PROP_STREAM:
      src->stream_id = g_value_get_uint (value);
      break;

    case PROP_COMPONENT:
      src->component_id = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gst_nice_src_get_property (
  GObject *object,
  guint prop_id,
  GValue *value,
  GParamSpec *pspec)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  switch (prop_id)
    {
    case PROP_AGENT:
      g_value_set_object (value, src->agent);
      break;

    case PROP_STREAM:
      g_value_set_uint (value, src->stream_id);
      break;

    case PROP_COMPONENT:
      g_value_set_uint (value, src->component_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nice_address_to_gst_net_address (NiceAddress *niceaddr, GstNetAddress *gstaddr)
{
  switch (niceaddr->s.addr.sa_family)
    {
    case AF_INET:
      gst_netaddress_set_ip4_address (gstaddr,
          niceaddr->s.ip4.sin_addr.s_addr,
          niceaddr->s.ip4.sin_port);
      break;
    case AF_INET6:
      gst_netaddress_set_ip6_address (gstaddr,
          niceaddr->s.ip6.sin6_addr.s6_addr,
          niceaddr->s.ip6.sin6_port);
      break;
    default:
      break;
    }
}

static void
new_selected_pair_cb (NiceAgent *agent, guint stream_id, guint component_id,
    gchar *local_cand, gchar *remote_cand, GstNiceSrc *src)
{
  GST_OBJECT_LOCK (src);

  if (stream_id == src->stream_id && component_id == src->component_id)
    {
      GSList *local_candidates = nice_agent_get_local_candidates (
          src->agent, stream_id, component_id);
      GSList *remote_candidates = nice_agent_get_remote_candidates (
          src->agent, stream_id, component_id);
      GSList *item = NULL;

      for (item = local_candidates; item; item = g_slist_next (item))
        {
          NiceCandidate *cand = item->data;
          if (!strcmp (local_cand, cand->foundation))
            {
              nice_address_to_gst_net_address (&cand->addr, &src->to);
              break;
            }
        }

      for (item = remote_candidates; item; item = g_slist_next (item))
        {
          NiceCandidate *cand = item->data;
          if (!strcmp (remote_cand, cand->foundation))
            {
              nice_address_to_gst_net_address (&cand->addr, &src->from);
              break;
            }
        }

      g_slist_foreach (local_candidates, (GFunc) nice_candidate_free, NULL);
      g_slist_free (local_candidates);
      g_slist_foreach (remote_candidates, (GFunc) nice_candidate_free, NULL);
      g_slist_free (remote_candidates);
    }

  GST_OBJECT_UNLOCK (src);
}

static GstStateChangeReturn
gst_nice_src_change_state (GstElement * element, GstStateChange transition)
{
  GstNiceSrc *src;
  GstStateChangeReturn ret;

  src = GST_NICE_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (src->agent == NULL || src->stream_id == 0 || src->component_id == 0)
        {
          GST_ERROR_OBJECT (element,
              "Trying to start Nice source without an agent set");
          return GST_STATE_CHANGE_FAILURE;
        }
      else
        {
          GST_OBJECT_LOCK (src);
          nice_agent_attach_recv (src->agent, src->stream_id, src->component_id,
              g_main_loop_get_context (src->mainloop),
              gst_nice_src_read_callback, (gpointer) src);

          if (!src->new_selected_pair_id)
            src->new_selected_pair_id = g_signal_connect (src->agent,
                "new-selected-pair", G_CALLBACK (new_selected_pair_cb), src);
          GST_OBJECT_UNLOCK (src);
        }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_OBJECT_LOCK (src);
      nice_agent_attach_recv (src->agent, src->stream_id, src->component_id,
          g_main_loop_get_context (src->mainloop), NULL, NULL);
      if (src->new_selected_pair_id)
        g_signal_handler_disconnect (src->agent, src->new_selected_pair_id);
      src->new_selected_pair_id = 0;
      GST_OBJECT_UNLOCK (src);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  return ret;
}


