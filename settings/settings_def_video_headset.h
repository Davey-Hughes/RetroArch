/* Single-source definitions: headset output group.
 * Grammar identical to settings_def_video_sync.h plus S_FLOAT and
 * the _NS no-sublabel variants; the descriptor argument span
 * matches SDESC_<kind>_ROW; row order is menu display order;
 * h2json.py parses these rows for the Crowdin source upload. */

S_BOOL(video_openxr_enable, VIDEO_OPENXR_ENABLE,
      "video_openxr_enable",
      false, SD_FLAG_CMD_APPLY_AUTO, 0, CMD_EVENT_REINIT,
      "Headset Output (OpenXR)",
      "Also show content in an OpenXR headset: each screen as its own floating screen, with both eyes of stereo 3D, and the menu in front of them. Vulkan only. The window keeps its normal output.")
S_FLOAT(video_openxr_distance, VIDEO_OPENXR_DISTANCE,
      "video_openxr_distance",
      1.8f, "%.1f m", SD_FLAG_ALLOW_INPUT, SDESC_RANGE_MINMAX, CMD_EVENT_NONE, 0.5, 10.0, 0.1, NULL, NULL,
      "Screen Distance",
      "How far in front of you the headset shows the screens.")
S_FLOAT(video_openxr_width, VIDEO_OPENXR_WIDTH,
      "video_openxr_width",
      1.6f, "%.1f m", SD_FLAG_ALLOW_INPUT, SDESC_RANGE_MINMAX, CMD_EVENT_NONE, 0.3, 10.0, 0.1, NULL, NULL,
      "Screen Width",
      "The width of the main screen in the headset. Other screens are sized to match it.")
