/*
 * Low Latency RTP video using GStreamer
 * =====================================
 *
 * A demo program that shows a low latency live h.264 stream from an IP camera
 * in a GTK application
 *
 * Code adapted from:
 *
 *   - https://gstreamer.freedesktop.org/documentation/tutorials/basic/toolkit-integration.html?gi-language=c
 *   - https://gstreamer.freedesktop.org/documentation/tutorials/basic/streaming.html?gi-language=c
 *   - https://valadoc.org/gstreamer-video-1.0/Gst.Video.Overlay.html#!
 *
 * Items of interest:
 *   
 *   - provide videosink with window pointer through callback (tell_window)
 *
 *   - the pipeline itself (create_pipeline). Creating directly compared to
 *     using gst_parse_launch() creates more control but also a lot more code
 *
 *     It uses several of the most important latency parameters for further
 *     experimentation
 *
 *   - Catching of qos messages, although not much is done with the data except
 *     printing it
 *
 *   - handoff signal from identity, again not much done except printing
 *
 *   - Latency is quite decent, observed 100..190ms end-to-end for a 5 megapixel
 *     30 fps stream. The 100 (actually 96) only once :) 150..160 more common.
 *     Tested on AMD Ryzen 5 2600/NVIDIA GeForce GTX 1060
 *
 * Todo:
 *   - Active supervision of the latency
 *   - Investigate whether this:
 *     http://gstreamer-devel.966125.n4.nabble.com/rtspsrc-jitterbuffer-stats-td4680812.html
 *     offers an optimization (partly done)
 *   - Investigate renew-stream messages to the source in case of decoder
 *     error
 *
 * Notes:
 *   - Some remaining features of the original sample don't make sense anymore
 *     (e.g. slider has no use on live input) or are left in broken condition
 *
 * Use at your own risk. Who's really into it can see I'm not
 *
 * 2021, Erik
 */

#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <gdk/gdk.h>
#if defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined (GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined (GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif

/* 
 * Structure to contain all our information, so we can pass it around 
 */

typedef struct _CustomData 
{
  GstElement*  pipeline;           /* Our one and only pipeline */

  gboolean     is_live;
  GtkWidget*   slider;              /* Slider widget to keep track of current position */
  GtkWidget*   streams_list;        /* Text widget to display info about the streams */
  gulong       slider_update_signal_id; /* Signal ID for the slider update signal */

  GstState     state;                 /* Current state of the pipeline */
  gint64       duration;                /* Duration of the clip, in nanoseconds */
  guintptr     window_handle;
  GstClockTime last_pts;
} 
CustomData;

/* This function is called when the GUI toolkit creates the physical window
 * that will hold the video.  At this point we can retrieve its handler (which
 * has a different meaning depending on the windowing system) and pass it to
 * GStreamer through the VideoOverlay interface. 
 */

static void realize_cb (GtkWidget *widget, CustomData *data) 
{
  GdkWindow *window = gtk_widget_get_window (widget);
  guintptr window_handle;

  if (!gdk_window_ensure_native (window))
  {
    g_error ("Couldn't create native window needed for GstVideoOverlay!");
  }

  /* Retrieve window handler from GDK */
#if defined (GDK_WINDOWING_WIN32)
  data->window_handle = (guintptr)GDK_WINDOW_HWND (window);
#elif defined (GDK_WINDOWING_QUARTZ)
  data->window_handle = gdk_quartz_window_get_nsview (window);
#elif defined (GDK_WINDOWING_X11)
  data->window_handle = GDK_WINDOW_XID (window);
#endif
}

/* 
 * Handlers for various button presses
 */
static void play_cb(GtkButton *button, CustomData *data) 
{
  gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
}

static void pause_cb(GtkButton *button, CustomData *data) 
{
  gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
}

static void stop_cb (GtkButton *button, CustomData *data) 
{
  gst_element_set_state(data->pipeline, GST_STATE_READY);
}

/* 
 * This function is called when the main window is closed 
 */

static void delete_event_cb(GtkWidget *widget, GdkEvent *event, CustomData *data) 
{
  stop_cb (NULL, data);
  gtk_main_quit ();
}

/* This function is called everytime the video window needs to be redrawn (due
 * to damage/exposure, rescaling, etc). GStreamer takes care of this in the
 * PAUSED and PLAYING states, otherwise, we simply draw a black rectangle to
 * avoid garbage showing up. 
 */

static gboolean draw_cb(GtkWidget *widget, cairo_t *cr, CustomData *data) 
{
	if (data->state < GST_STATE_PAUSED)
	{
		GtkAllocation allocation;

		/* Cairo is a 2D graphics library which we use here to clean the video window.
		 * It is used by GStreamer for other reasons, so it will always be available to us. */
		gtk_widget_get_allocation (widget, &allocation);
		cairo_set_source_rgb (cr, 0, 0, 0);
		cairo_rectangle (cr, 0, 0, allocation.width, allocation.height);
		cairo_fill (cr);
	}
	return FALSE;
}

/* 
 * This function is called when the slider changes its position. We perform a
 * seek to the new position here. 
 */

static void slider_cb (GtkRange *range, CustomData *data) 
{
  gdouble value = gtk_range_get_value (GTK_RANGE (data->slider));
  gst_element_seek_simple (data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
      (gint64)(value * GST_SECOND));
}

/* 
 * This creates all the GTK+ widgets that compose our application, and registers the callbacks 
 */

static void create_ui (CustomData *data) 
{
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GtkWidget *video_window; /* The drawing area where the video will be shown */
  GtkWidget *main_box;     /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;    /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;     /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button; /* Buttons */

  main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);

  video_window = gtk_drawing_area_new ();
  gtk_widget_set_double_buffered (video_window, FALSE);
  g_signal_connect (video_window, "realize", G_CALLBACK (realize_cb), data);
  g_signal_connect (video_window, "draw", G_CALLBACK (draw_cb), data);

  play_button = gtk_button_new_from_icon_name ("media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (play_button), "clicked", G_CALLBACK (play_cb), data);

  pause_button = gtk_button_new_from_icon_name ("media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (pause_button), "clicked", G_CALLBACK (pause_cb), data);

  stop_button = gtk_button_new_from_icon_name ("media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (stop_cb), data);

  data->slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value (GTK_SCALE (data->slider), 0);
  data->slider_update_signal_id = g_signal_connect (G_OBJECT (data->slider), "value-changed", G_CALLBACK (slider_cb), data);

  data->streams_list = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (data->streams_list), FALSE);

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), data->slider, TRUE, TRUE, 2);

  main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), data->streams_list, FALSE, FALSE, 2);

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (main_window), 640, 480);

  gtk_widget_show_all (main_window);
}

/* 
 * Called every second to print some time info
 */

static gboolean update_timeinfo(CustomData *data) 
{
  gint64 current = -1;

  /* We do not want to update anything unless we are in the PAUSED or PLAYING states */
  if (data->state < GST_STATE_PAUSED)
  {
    return TRUE;
  }

  if (gst_element_query_position(data->pipeline, GST_FORMAT_TIME, &current)) 
  {

     GstClockTimeDiff diff = GST_CLOCK_DIFF(current, data->last_pts);
     g_print("Last PTS: %" GST_TIME_FORMAT ", current: %" GST_TIME_FORMAT ", Diff with current: %li.%03lims\n", GST_TIME_ARGS(data->last_pts), GST_TIME_ARGS(current), diff / 1000000, (labs(diff) / 1000) % 1000);
  }
  return TRUE;
}

/* 
 * An error message was posted on the bus 
 */

static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) 
{
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state (data->pipeline, GST_STATE_READY);
}

/* 
 * This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) 
 */

static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("End-Of-Stream reached.\n");
  gst_element_set_state (data->pipeline, GST_STATE_READY);
}

static void handoff_cb(GstElement* identity, GstBuffer* buffer, CustomData *data)
{
  data->last_pts = GST_BUFFER_PTS(buffer);
}

/* 
 * This function is called when the pipeline changes states. We use it to
 * keep track of the current state. 
 */

static void state_changed_cb(GstBus *bus, GstMessage *msg, CustomData *data) 
{
   GstState old_state, new_state, pending_state;
   gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);

   data->state = new_state;
   g_print ("State set to %s\n", gst_element_state_get_name (new_state));
   if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) 
   {
      /* Refresh the GUI as soon as we reach the PAUSED state */
      update_timeinfo(data);
   }
}

/*
 * See GstBusSyncHandler documentation
 *
 * Note: user_data must be valid pointer to CustomData
 */

static GstBusSyncReply tell_window(GstBus * bus, GstMessage * message, CustomData* data)
{
   // ignore anything but 'prepare-window-handle' element messages
   if (!gst_is_video_overlay_prepare_window_handle_message(message))
   {
      return GST_BUS_PASS;
   }

   gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message)), data->window_handle);
   gst_message_unref (message);
   return GST_BUS_DROP;
}

/* 
 * This function is called when an "application" message is posted on the bus.
 * Here we retrieve the message posted by the tags_cb callback 
 */

static void application_cb(GstBus *bus, GstMessage *msg, CustomData *data) 
{
	if (g_strcmp0 (gst_structure_get_name (gst_message_get_structure (msg)), "tags-changed") == 0) 
	{
		/* removed */
	}
}

/*
 * QOS message sent on the bus. For now we don't do much, just print.
 *
 * But it's a starting point for more specific handling...
 */

static void qos_cb(GstBus *bus, GstMessage *msg, CustomData *data) 
{
   guint64 running_time;
   guint64 stream_time;
   guint64 timestamp;
   guint64 duration;
   gboolean live;

   GstFormat format;
   guint64 processed;
   guint64 dropped;

   gint64 jitter;
   gdouble proportion;
   gint quality;

   gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);
   gst_message_parse_qos_stats (msg, &format, &processed, &dropped);
   gst_message_parse_qos_values (msg, &jitter, &proportion, &quality);

   g_print(
         "QOS! running_time: %" GST_TIME_FORMAT ", stream_time: %" GST_TIME_FORMAT 
         ", ts: %" GST_TIME_FORMAT ", duration: %" GST_TIME_FORMAT
         ", processed: %lu, dropped: %lu, jitter: %li\n",
         GST_TIME_ARGS(running_time), 
         GST_TIME_ARGS(stream_time),
         GST_TIME_ARGS(timestamp),
         GST_TIME_ARGS(duration),
         processed,
         dropped,
         jitter
         );

}

/*
 * Handler for dynamic adding of rtsp pad, which only appears after
 * initialization. See:
 *
 * https://gstreamer.freedesktop.org/documentation/application-development/basics/pads.html
 *
 * Credits: https://stackoverflow.com/questions/32233370/
 */

static void rtsp_pad_added_cb(GstElement* element, GstPad* pad, gpointer data)
{
   gchar* name = gst_pad_get_name(pad);
   if (!gst_element_link_pads(element, name, GST_ELEMENT(data), "sink"))
   {
      printf("Failed to link elements\n");
   }
   g_free(name);
}

/*
 * Create video pipeline and take care of naming all the elements. It replaces
 * 
 * gst_parse_launch("rtspsrc location=<url> user-id=<user> user-pw=<pw> latency=0 ! 
 * rtpjitterbuffer latency=80 ! rtph264depay ! avdec_h264 ! identity ! autovideosink", NULL);
 *
 * and brings the advantage (later on) that we can address each element
 * individually and add the callback to the identity more easily
 *
 * Note that rtspsrc has an rtpjitterbuffer inside so you shouldn't insert it
 * in the pipeline like above
 *
 * https://gstreamer.freedesktop.org/documentation/additional/rtp.html?gi-language=c
 * https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html?gi-language=c#rtspsrc-page
 *
 */

static GstElement* create_pipeline(const char* pipeline_prefix, const char *url, const char* username, const char* password, void* custom_data)
{
   char buf[64];
   int offs = strlen(pipeline_prefix);

   if (offs < sizeof(buf))
   {
      strcpy(buf, pipeline_prefix);
      strcpy(buf+offs, "videoplayer");
      GstElement* pipeline = gst_pipeline_new(buf);

      strcpy(buf+offs, "source");
      GstElement* rtp_source = gst_element_factory_make ("rtspsrc", buf);
      /*
       * Setting ntp-time-source=2 removes considerable latency in case of no
       * timesync. It makes it as nearly fast as Low Latency Viewer, the
       * latency value for dejitter being the only difference
       */
      g_object_set(G_OBJECT(rtp_source), "location", url, "user-id", username, "user-pw", password, "latency", 20, "ntp-time-source", 2, NULL);

      strcpy(buf+offs, "depay");
      GstElement* depay = gst_element_factory_make ("rtph264depay", buf);
      strcpy(buf+offs, "decoder");
      GstElement* decoder = gst_element_factory_make ("avdec_h264", buf);
      strcpy(buf+offs, "identity");
      GstElement* identity = gst_element_factory_make ("identity", buf);
      strcpy(buf+offs, "sink");
      GstElement* sink = gst_element_factory_make ("xvimagesink", buf);
      // g_object_set(G_OBJECT(sink), "sync", FALSE, NULL);
      g_object_set(G_OBJECT(sink), "qos", TRUE, NULL);
      g_object_set(G_OBJECT(sink), "render-delay", 0, NULL);

      if (rtp_source && depay && decoder && identity && sink)
      {
         gst_bin_add_many(GST_BIN(pipeline), rtp_source, depay, decoder, identity, sink, NULL);
         g_signal_connect(rtp_source, "pad-added", G_CALLBACK(rtsp_pad_added_cb), depay);
         if (gst_element_link_many(depay, decoder, identity, sink, NULL)) 
         {
            // https://stackoverflow.com/questions/45079457/gstreamer-buffer-pts#45083086
            g_signal_connect(identity, "handoff", G_CALLBACK(handoff_cb), custom_data);
            return pipeline;
         }
         g_warning("Failed to link elements!");
      }
      else
      {
         g_warning("Failed to create one or more elements!");
      }
   }
   return NULL;
}

int main(int argc, char *argv[]) 
{
   CustomData data;

   GstStateChangeReturn ret;
   GstBus *bus;

   gtk_init (&argc, &argv);
   gst_init (&argc, &argv);
   memset(&data, 0, sizeof (data));
   data.duration = GST_CLOCK_TIME_NONE;

   /* Create the GUI (and save the window pointer) */
   create_ui(&data);

   // data.pipeline = gst_parse_launch ("rtspsrc location=rtsp://192.168.0.33/axis-media/media.amp?resolution=1280x720 user-id=root user-pw=pass latency=40 ! rtph264depay ! avdec_h264 ! identity ! autovideosink", NULL);
   data.pipeline = create_pipeline("input1-", argv[1], "root", "pass", &data);
   if (!data.pipeline) 
   {
      g_printerr ("Error creating pipeline\n");
      return -1;
   }

   bus = gst_element_get_bus(data.pipeline);
   gst_bus_set_sync_handler(bus, (GstBusSyncHandler) tell_window, &data, NULL);
   gst_bus_add_signal_watch(bus);

   g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
   g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, &data);
   g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)state_changed_cb, &data);
   g_signal_connect (G_OBJECT (bus), "message::qos", (GCallback)qos_cb, &data);
   gst_object_unref (bus);

   /* Start playing */
   ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
   switch (ret)
   {
   case GST_STATE_CHANGE_FAILURE:
      {
         g_printerr ("Unable to set the pipeline to the playing state.\n");
         gst_object_unref (data.pipeline);
         return -1;
      }
   case GST_STATE_CHANGE_NO_PREROLL:
      /* Got this from basic-tutorial-12 but it doesn't seem to work */
      data.is_live = TRUE;
      break;
   default:
      break;
   }

   g_timeout_add_seconds(1, (GSourceFunc)update_timeinfo, &data);

   gtk_main ();

   gst_element_set_state(data.pipeline, GST_STATE_NULL);
   gst_object_unref(data.pipeline);
   return 0;
}

/* vim: set nowrap sw=3 sts=3 et fdm=marker: */
