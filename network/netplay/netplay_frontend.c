/* RetroArch - GekkoNet backed netplay frontend */
/* Replaces legacy RetroArch netplay stack with thin GekkoNet integration. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <features/features_cpu.h>

#include <boolean.h>
#include <string/stdstring.h>

#include "netplay.h"
#include "netplay_defines.h"

#include "../../configuration.h"
#include "../../tasks/task_content.h"
#include "../../tasks/tasks_internal.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../performance_counters.h"
#include "../../input/input_driver.h"
#include "../../runloop.h"

#if defined(_WIN32)
#include "../../gekkonet/windows/include/gekkonet.h"
#elif defined(__APPLE__)
#include "../../gekkonet/mac/include/gekkonet.h"
#else
#include "../../gekkonet/linux/include/gekkonet.h"
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

/* Forward declarations from runloop.c */
bool core_set_netplay_callbacks(void);
bool core_unset_netplay_callbacks(void);

/* ------------------------------------------------------------------------- */
/* MITM server table retained for UI compatibility. */
const mitm_server_t netplay_mitm_server_list[NETPLAY_MITM_SERVERS] = {
   { "nyc",       MENU_ENUM_LABEL_VALUE_NETPLAY_MITM_SERVER_LOCATION_1 },
   { "madrid",    MENU_ENUM_LABEL_VALUE_NETPLAY_MITM_SERVER_LOCATION_2 },
   { "saopaulo",  MENU_ENUM_LABEL_VALUE_NETPLAY_MITM_SERVER_LOCATION_3 },
   { "singapore", MENU_ENUM_LABEL_VALUE_NETPLAY_MITM_SERVER_LOCATION_4 },
   { "custom",    MENU_ENUM_LABEL_VALUE_NETPLAY_MITM_SERVER_LOCATION_CUSTOM }
};

/* ------------------------------------------------------------------------- */
/* Simplified GekkoNet backed state */
typedef struct gekko_netplay_state
{
   GekkoSession      *session;
   GekkoNetAdapter   *adapter;
   GekkoConfig        config;
   int                local_handle;
   int                remote_handle;
   GekkoNetAddress    remote_addr;
   unsigned short     listen_port;
   bool               is_server;
   bool               running;
   bool               paused;
   bool               callbacks_installed;
   bool               session_ready;
   bool               session_warned;
   retro_time_t       session_start_time;
   /* aggregated per-player input for the current frame */
   struct
   {
      uint16_t buttons;
      int16_t  lx;
      int16_t  ly;
   }                  player_inputs[4];
   unsigned char      num_players;
   bool               inputs_ready;
   int                current_frame;
   /* simple cached per-player inputs */
   uint16_t           last_buttons;
   int16_t            last_lx;
   int16_t            last_ly;
} gekko_netplay_state_t;

static net_driver_state_t networking_driver_st;
static gekko_netplay_state_t g_gekkonet;
/* Netplay hosting now requires an explicit user request (via menu/hotkey).
 * Track when a host start was actually asked for so we can ignore any
 * implicit/automatic initialization attempts. */
static bool g_host_start_requested;

/* The default GekkoNet adapter binds a UDP port when created. We keep a single
 * cached instance alive for the lifetime of RetroArch so we don't attempt to
 * bind the same port more than once when users start/stop sessions repeatedly
 * during the same run (which resulted in `EADDRINUSE`). */
static GekkoNetAdapter *g_cached_adapter;
static unsigned short   g_cached_adapter_port;

static uint32_t gekkonet_checksum(const unsigned char *data, unsigned int len)
{
   /* Lightweight FNV-1a checksum for desync detection. */
   uint32_t hash = 2166136261u;
   unsigned int i;
   for (i = 0; i < len; i++)
   {
      hash ^= data[i];
      hash *= 16777619u;
   }
   return hash;
}

static unsigned gekkonet_local_port(void)
{
   /* Host controls player 1 (port 0), client controls player 2 (port 1). */
   return g_gekkonet.is_server ? 0 : 1;
}

static uint16_t gekkonet_read_buttons(void);

static void gekkonet_install_callbacks(void)
{
   if (g_gekkonet.callbacks_installed)
      return;

   if (core_set_netplay_callbacks())
      g_gekkonet.callbacks_installed = true;
}

static void gekkonet_uninstall_callbacks(void)
{
   if (!g_gekkonet.callbacks_installed)
      return;

   core_unset_netplay_callbacks();
   g_gekkonet.callbacks_installed = false;
}

static void gekkonet_free_remote_addr(void)
{
   if (g_gekkonet.remote_addr.data)
      free(g_gekkonet.remote_addr.data);

   g_gekkonet.remote_addr.data = NULL;
   g_gekkonet.remote_addr.size = 0;
}

static void gekkonet_reset_state(void)
{
   gekkonet_free_remote_addr();
   memset(&g_gekkonet, 0, sizeof(g_gekkonet));
}

static bool gekkonet_serialize_state(unsigned char *dst,
      unsigned int capacity, unsigned int *out_len)
{
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   size_t serialize_size       = 0;

   if (!runloop_st
         || !runloop_st->current_core.retro_serialize
         || !runloop_st->current_core.retro_serialize_size)
      return false;

   serialize_size = runloop_st->current_core.retro_serialize_size();
   if (!serialize_size || serialize_size > capacity)
      return false;

   if (!runloop_st->current_core.retro_serialize(dst, serialize_size))
      return false;

   if (out_len)
      *out_len = (unsigned)serialize_size;
   return true;
}

static bool gekkonet_load_state(const unsigned char *src, unsigned int len)
{
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   if (!runloop_st || !runloop_st->current_core.retro_unserialize)
      return false;
   return runloop_st->current_core.retro_unserialize(src, len);
}

net_driver_state_t *networking_state_get_ptr(void)
{
   return &networking_driver_st;
}

static GekkoNetAdapter *gekkonet_get_adapter(unsigned short port)
{
   /* Create a fresh adapter each time. The GekkoNet default adapter will bind
    * the requested port and return NULL on failure. */
   return gekko_default_adapter(port);
}

static void gekkonet_start_nat_traversal(unsigned short port)
{
   net_driver_state_t *net_st = networking_state_get_ptr();

   if (!net_st)
      return;

   if (!task_push_netplay_nat_traversal(&net_st->nat_traversal_request, port))
      RARCH_WARN("[GekkoNet] NAT traversal setup failed to start.\n");
   else
      RARCH_LOG("[GekkoNet] NAT traversal requested for port %u.\n", port);
}

static bool gekkonet_resolve_remote(const char *server, unsigned port)
{
   char port_buf[16];
   struct addrinfo hints;
   struct addrinfo *res = NULL;

   if (!server || string_is_empty(server))
      return false;

   snprintf(port_buf, sizeof(port_buf), "%u", port);

   memset(&hints, 0, sizeof(hints));
   hints.ai_family   = AF_UNSPEC;
   hints.ai_socktype = SOCK_DGRAM;

   if (getaddrinfo(server, port_buf, &hints, &res) != 0 || !res)
   {
      RARCH_ERR("[GekkoNet] Failed to resolve remote host '%s:%u'.\n", server, port);
      if (res)
         freeaddrinfo(res);
      return false;
   }

   g_gekkonet.remote_addr.data = malloc(res->ai_addrlen);
   if (!g_gekkonet.remote_addr.data)
   {
      RARCH_ERR("[GekkoNet] Failed to allocate remote address buffer.\n");
      freeaddrinfo(res);
      return false;
   }

   memcpy(g_gekkonet.remote_addr.data, res->ai_addr, res->ai_addrlen);
   g_gekkonet.remote_addr.size = (unsigned int)res->ai_addrlen;

   freeaddrinfo(res);
   return true;
}

static void gekkonet_update_inputs(const GekkoGameEvent *evt)
{
   unsigned int per_player = g_gekkonet.config.input_size;
   unsigned int i;

   if (!evt || evt->type != AdvanceEvent)
      return;

   if (!evt->data.adv.inputs || evt->data.adv.input_len < per_player)
      return;

   for (i = 0; i < g_gekkonet.num_players; i++)
   {
      unsigned int offset = i * per_player;
      if (offset + sizeof(g_gekkonet.player_inputs[i]) <= evt->data.adv.input_len)
      {
         memcpy(&g_gekkonet.player_inputs[i],
               evt->data.adv.inputs + offset,
               sizeof(g_gekkonet.player_inputs[i]));
      }
   }

   g_gekkonet.inputs_ready  = true;
   g_gekkonet.current_frame = evt->data.adv.frame;
   RARCH_LOG("[GekkoNet] AdvanceEvent frame %d (len %u, rollback=%s).\n",
         evt->data.adv.frame,
         evt->data.adv.input_len,
         evt->data.adv.rolling_back ? "yes" : "no");
}

static void gekkonet_handle_save_event(GekkoGameEvent *evt)
{
   unsigned int written = 0;
   size_t cur_serialize_sz = 0;
   runloop_state_t *runloop_st = runloop_state_get_ptr();

   if (!evt)
      return;

   if (!g_gekkonet.config.state_size)
      return;

   if (runloop_st && runloop_st->current_core.retro_serialize_size)
      cur_serialize_sz = runloop_st->current_core.retro_serialize_size();

   if (cur_serialize_sz > g_gekkonet.config.state_size)
   {
      RARCH_ERR("[GekkoNet] Serialize size %u exceeds buffer %u; cannot save state.\n",
            (unsigned)cur_serialize_sz, g_gekkonet.config.state_size);
      RARCH_ERR("[GekkoNet] Increase rewind_buffer_size or set a larger state buffer.\n");
      return;
   }

   if (!gekkonet_serialize_state(evt->data.save.state,
            g_gekkonet.config.state_size, &written))
   {
      if (evt->data.save.state_len)
         *evt->data.save.state_len = 0;
      if (evt->data.save.checksum)
         *evt->data.save.checksum = 0;
      RARCH_ERR("[GekkoNet] Serialize failed for frame %d.\n", evt->data.save.frame);
      return;
   }

   if (evt->data.save.state_len)
      *evt->data.save.state_len = written;
   if (evt->data.save.checksum)
      *evt->data.save.checksum = gekkonet_checksum(evt->data.save.state, written);
}

static void gekkonet_handle_load_event(GekkoGameEvent *evt)
{
   if (!evt || !evt->data.load.state || !evt->data.load.state_len)
      return;

   if (!gekkonet_load_state(evt->data.load.state, evt->data.load.state_len))
      RARCH_ERR("[GekkoNet] Failed to load state for frame %d.\n",
            evt->data.load.frame);
   else
      RARCH_LOG("[GekkoNet] Loaded state for frame %d (len %u).\n",
            evt->data.load.frame, evt->data.load.state_len);
}

static void gekkonet_process_game_events(void)
{
   int event_count       = 0;
   int session_event_cnt = 0;
   GekkoGameEvent **events = NULL;
   int i;

   events = gekko_update_session(g_gekkonet.session, &event_count);

   if (events && event_count > 0)
   {
      for (i = 0; i < event_count; i++)
      {
         GekkoGameEvent *evt = events[i];
         if (!evt)
            continue;

         switch (evt->type)
         {
            case AdvanceEvent:
               gekkonet_update_inputs(evt);
               break;
            case SaveEvent:
               gekkonet_handle_save_event(evt);
               break;
            case LoadEvent:
               gekkonet_handle_load_event(evt);
               break;
            default:
               break;
         }
      }
   }

   /* Drain session events to avoid back pressure, even if we ignore them. */
   {
      int j;
      GekkoSessionEvent **session_events =
         gekko_session_events(g_gekkonet.session, &session_event_cnt);

      if (session_events && session_event_cnt > 0)
      {
         for (j = 0; j < session_event_cnt; j++)
         {
            GekkoSessionEvent *sevt = session_events[j];
            if (!sevt)
               continue;

            switch (sevt->type)
            {
               case PlayerConnected:
                  RARCH_LOG("[GekkoNet] Peer connected (handle %d).\n",
                        sevt->data.connected.handle);
                  break;
               case PlayerDisconnected:
                  RARCH_WARN("[GekkoNet] Peer disconnected (handle %d).\n",
                        sevt->data.disconnected.handle);
                  break;
               case PlayerSyncing:
                  RARCH_LOG("[GekkoNet] Syncing peer %d (%u/%u).\n",
                        sevt->data.syncing.handle,
                        sevt->data.syncing.current,
                        sevt->data.syncing.max);
                  break;
            case SessionStarted:
               g_gekkonet.session_ready = true;
               RARCH_LOG("[GekkoNet] Session synchronized.\n");
               break;
               case DesyncDetected:
                  RARCH_WARN("[GekkoNet] Desync detected at frame %d (local %u, remote %u, peer %d).\n",
                        sevt->data.desynced.frame,
                        sevt->data.desynced.local_checksum,
                        sevt->data.desynced.remote_checksum,
                        sevt->data.desynced.remote_handle);
                  break;
               default:
                  break;
            }
         }
      }
   }

   /* Connection progress logging for clients waiting to sync */
   if (g_gekkonet.running && !g_gekkonet.session_ready && !g_gekkonet.session_warned)
   {
      retro_time_t now = cpu_features_get_time_usec();
      /* Warn if we have waited ~5 seconds without session start */
      if (now - g_gekkonet.session_start_time > (retro_time_t)5000000)
      {
         RARCH_WARN("[GekkoNet] Still waiting for peer/session sync...\n");
         g_gekkonet.session_warned = true;
      }
   }
}

static void gekkonet_step_frame(void)
{
   if (!g_gekkonet.running)
      return;

   /* Push local inputs for this frame, then advance the session. */
   {
      unsigned local_port = gekkonet_local_port();
      uint16_t buttons = gekkonet_read_buttons();
      int16_t lx       = input_driver_state_wrapper(local_port, RETRO_DEVICE_ANALOG,
            RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      int16_t ly       = input_driver_state_wrapper(local_port, RETRO_DEVICE_ANALOG,
            RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

      struct
      {
         uint16_t buttons;
         int16_t  lx;
         int16_t  ly;
      } payload;

      payload.buttons     = buttons;
      payload.lx          = lx;
      payload.ly          = ly;

      g_gekkonet.last_buttons = buttons;
      g_gekkonet.last_lx      = lx;
      g_gekkonet.last_ly      = ly;

      gekko_add_local_input(g_gekkonet.session,
            g_gekkonet.local_handle, &payload);
   }

   gekko_network_poll(g_gekkonet.session);
   gekkonet_process_game_events();
}

static uint16_t gekkonet_read_buttons(void)
{
   /* Use joypad mask for player 0 */
   unsigned local_port = gekkonet_local_port();
   /* Use joypad mask for the appropriate local player */
   return (uint16_t)input_driver_state_wrapper(local_port, RETRO_DEVICE_JOYPAD,
            0, RETRO_DEVICE_ID_JOYPAD_MASK);
}

static bool gekkonet_init_session(bool is_server, const char *server, unsigned port)
{
   settings_t *settings = config_get_ptr();
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   size_t serialize_sz         = 0;
   size_t fallback_state_sz    = 0;
   unsigned char desired_players;
   unsigned short adapter_port  = (unsigned short)port;

   gekkonet_reset_state();

   /* GekkoNet needs one local + one remote player. Ensure we always allocate
    * for at least two slots even if the frontend is configured for a single
    * local user. */
   desired_players        = settings->uints.input_max_users;
   if (desired_players < 2)
      desired_players = 2;
   else if (desired_players > 4)
      desired_players = 4;
   g_gekkonet.num_players = desired_players;

   if (runloop_st && runloop_st->current_core.retro_serialize_size)
      serialize_sz = runloop_st->current_core.retro_serialize_size();
   else
      RARCH_WARN("[GekkoNet] Core serialization size unavailable; using fallback buffer.\n");

   /* Use a generous default to cover large core states. Ignore tiny rewind buffers. */
   fallback_state_sz = 128 * 1024 * 1024; /* 128 MiB default */

   if (!gekko_create(&g_gekkonet.session))
   {
      RARCH_ERR("[GekkoNet] Failed to create session.\n");
      return false;
   }

   g_gekkonet.adapter     = gekkonet_get_adapter(adapter_port);
   g_gekkonet.listen_port = adapter_port;
   g_gekkonet.is_server   = is_server;
   g_gekkonet.inputs_ready= false;
   g_gekkonet.paused      = false;
   g_gekkonet.session_ready = false;
   g_gekkonet.session_warned= false;
   g_gekkonet.session_start_time = cpu_features_get_time_usec();

   if (!g_gekkonet.adapter)
   {
      if (!is_server)
      {
         /* Client fallback: let OS choose a free port if the requested one is busy. */
         adapter_port       = 0;
         g_gekkonet.adapter = gekkonet_get_adapter(adapter_port);
         g_gekkonet.listen_port = adapter_port;
         if (g_gekkonet.adapter)
            RARCH_WARN("[GekkoNet] Requested client port %u in use; falling back to ephemeral port.\n", port);
      }

      if (!g_gekkonet.adapter)
      {
         RARCH_ERR("[GekkoNet] Failed to create default adapter on port %u.\n", port);
         return false;
      }
   }

   memset(&g_gekkonet.config, 0, sizeof(g_gekkonet.config));
   g_gekkonet.config.num_players             = g_gekkonet.num_players;
   g_gekkonet.config.max_spectators          = 0;
   g_gekkonet.config.input_prediction_window = 2;
   g_gekkonet.config.spectator_delay         = 0;
   g_gekkonet.config.input_size              = sizeof(uint16_t) + sizeof(int16_t) * 2;
   {
      size_t chosen_sz = serialize_sz;
      if (fallback_state_sz > chosen_sz)
         chosen_sz = fallback_state_sz;
      g_gekkonet.config.state_size = (unsigned)chosen_sz;
      if (serialize_sz)
         RARCH_LOG("[GekkoNet] State buffer set to %u bytes (serialize size %u, fallback %u).\n",
               (unsigned)chosen_sz, (unsigned)serialize_sz, (unsigned)fallback_state_sz);
      else
         RARCH_LOG("[GekkoNet] State buffer set to %u bytes (fallback; serialize size unknown).\n",
               (unsigned)chosen_sz);
   }
   g_gekkonet.config.limited_saving          = false;
   g_gekkonet.config.post_sync_joining       = false;
   g_gekkonet.config.desync_detection        = true;

   if (!g_gekkonet.config.state_size)
   {
      g_gekkonet.config.state_size = 1024 * 1024;
      RARCH_WARN("[GekkoNet] State size unavailable; defaulting to %u bytes.\n",
            g_gekkonet.config.state_size);
   }

   gekko_net_adapter_set(g_gekkonet.session, g_gekkonet.adapter);

   g_gekkonet.local_handle  = gekko_add_actor(g_gekkonet.session, LocalPlayer, NULL);

   if (is_server)
   {
      g_gekkonet.remote_handle = gekko_add_actor(g_gekkonet.session, RemotePlayer, NULL);
      /* Request NAT traversal for hosts to publish external mapping. */
      gekkonet_start_nat_traversal((unsigned short)port);
   }
   else
   {
      if (!gekkonet_resolve_remote(server, port))
      {
         RARCH_ERR("[GekkoNet] Unable to resolve remote host for client session.\n");
         return false;
      }

      g_gekkonet.remote_handle = gekko_add_actor(g_gekkonet.session, RemotePlayer,
            &g_gekkonet.remote_addr);
   }

   gekko_start(g_gekkonet.session, &g_gekkonet.config);
   g_gekkonet.running = true;

   RARCH_LOG("[GekkoNet] Netplay session started on port %u (%s).\n", adapter_port,
         is_server ? "host" : "client");

   return true;
}

static void gekkonet_shutdown(void)
{
   if (g_gekkonet.adapter || g_gekkonet.session)
      RARCH_LOG("[GekkoNet] Shutting down session.\n");

   if (g_gekkonet.session)
      gekko_destroy(g_gekkonet.session);

   gekkonet_uninstall_callbacks();
   gekkonet_free_remote_addr();

   gekkonet_reset_state();
}

/* ------------------------------------------------------------------------- */
/* Discovery stubs */
#ifdef HAVE_NETPLAYDISCOVERY
bool init_netplay_discovery(void)
{
   return false;
}

void deinit_netplay_discovery(void)
{
}

bool netplay_discovery_driver_ctl(enum rarch_netplay_discovery_ctl_state state,
      void *data)
{
   (void)state;
   (void)data;
   return false;
}
#endif

/* ------------------------------------------------------------------------- */
/* Public API expected by the rest of RetroArch */

bool netplay_compatible_version(const char *version)
{
   /* With GekkoNet we defer compatibility to the SDK; accept all versions here. */
   (void)version;
   return true;
}

bool netplay_decode_hostname(const char *hostname,
      char *address, unsigned *port, char *session_name, size_t len)
{
   const char *colon;
   size_t address_len = 0;

   if (!hostname || !address || !port)
      return false;

   colon = strchr(hostname, ':');
   if (colon)
   {
      address_len = (size_t)(colon - hostname);
      if (address_len >= len)
         address_len = len ? len - 1 : 0;

      if (len && address_len > 0)
      {
         memcpy(address, hostname, address_len);
         address[address_len] = '\0';
      }
      else if (len)
         address[0] = '\0';

      *port = (unsigned)strtoul(colon + 1, NULL, 10);
   }
   else
   {
      if (len)
         strlcpy(address, hostname, len);
      *port = 55435; /* default */
   }

   if (session_name)
   {
      if (len)
         session_name[0] = '\0';
   }

   return true;
}

bool init_netplay_deferred(const char *server, unsigned port,
      const char *mitm_session)
{
   net_driver_state_t *net_st = networking_state_get_ptr();

   (void)mitm_session;

   if (!net_st)
      return false;

   strlcpy(net_st->server_address_deferred, server ? server : "", sizeof(net_st->server_address_deferred));
   net_st->server_port_deferred = port;
   net_st->flags               |= (1 << 0);

   return true;
}

bool init_netplay(const char *server, unsigned port, const char *mitm_session)
{
   (void)mitm_session;

   /* Only honor host startup when explicitly requested. */
   if ((server == NULL || string_is_empty(server)) && !g_host_start_requested)
      return false;

   g_host_start_requested = false;

   if (g_gekkonet.running)
      return true;

   g_gekkonet.session_start_time = cpu_features_get_time_usec();
   g_gekkonet.session_ready      = false;
   g_gekkonet.session_warned     = false;

   if (!gekkonet_init_session(server == NULL || string_is_empty(server), server, port))
      return false;

   networking_driver_st.data = (netplay_t*)&g_gekkonet;
   gekkonet_install_callbacks();

   return true;
}

void deinit_netplay(void)
{
   gekkonet_shutdown();
   networking_driver_st.data = NULL;
}

bool netplay_reinit_serialization(void)
{
   /* GekkoNet manages its own rollback buffers. */
   return true;
}

bool netplay_is_spectating(void)
{
   return false;
}

void netplay_force_send_savestate(void)
{
}

int16_t netplay_input_state(unsigned port, unsigned device,
      unsigned idx, unsigned id)
{
   /* If netplay is disabled, just use local input. */
   if (!g_gekkonet.running)
      return input_driver_state_wrapper(port, device, idx, id);

   /* If we haven't received aggregated inputs yet, fall back to local input
    * for the first player so gameplay isn't blocked while syncing. */
   if (!g_gekkonet.inputs_ready)
   {
      unsigned local_port = gekkonet_local_port();
      if (port == local_port)
         return input_driver_state_wrapper(local_port, device, idx, id);
      return 0;
   }

   if (port < g_gekkonet.num_players)
   {
      uint16_t buttons = g_gekkonet.player_inputs[port].buttons;
      switch (device)
      {
         case RETRO_DEVICE_JOYPAD:
            if (id < 32)
               return (buttons & (1u << id)) ? 1 : 0;
            return 0;
         case RETRO_DEVICE_ANALOG:
            if (idx == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            {
               if (id == RETRO_DEVICE_ID_ANALOG_X)
                  return g_gekkonet.player_inputs[port].lx;
               if (id == RETRO_DEVICE_ID_ANALOG_Y)
                  return g_gekkonet.player_inputs[port].ly;
            }
            /* No right stick data in our compact payload */
            return 0;
         default:
            break;
      }
   }

   return input_driver_state_wrapper(port, device, idx, id);
}

bool netplay_driver_ctl(enum rarch_netplay_ctl_state state, void *data)
{
   bool ret = true;
   switch (state)
   {
      case RARCH_NETPLAY_CTL_IS_ENABLED:
         ret = g_gekkonet.running;
         break;
      case RARCH_NETPLAY_CTL_IS_SERVER:
         ret = g_gekkonet.running && g_gekkonet.is_server;
         break;
      case RARCH_NETPLAY_CTL_IS_CONNECTED:
         ret = g_gekkonet.running;
         break;
      case RARCH_NETPLAY_CTL_IS_PLAYING:
         ret = g_gekkonet.running;
         break;
      case RARCH_NETPLAY_CTL_IS_SPECTATING:
         ret = false;
         break;
      case RARCH_NETPLAY_CTL_ENABLE_SERVER:
      {
         settings_t *settings = config_get_ptr();
         g_host_start_requested = true;

         /* If there is no active core/content, defer session creation until the
          * next content load triggers CMD_EVENT_NETPLAY_INIT. */
         if (!content_is_inited())
            ret = true;
         else
            ret = init_netplay(NULL, settings->uints.netplay_port, NULL);
         break;
      }
      case RARCH_NETPLAY_CTL_ENABLE_CLIENT:
      {
         settings_t *settings = config_get_ptr();
         ret = init_netplay(settings->paths.netplay_server,
               settings->uints.netplay_port, settings->paths.netplay_password);
         break;
      }
      case RARCH_NETPLAY_CTL_DISABLE:
         deinit_netplay();
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_DISCONNECT:
         deinit_netplay();
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_PRE_FRAME:
         if (g_gekkonet.running)
         {
            if (g_gekkonet.paused)
            {
               /* Keep the session breathing while paused, but don't block the runloop. */
               gekko_network_poll(g_gekkonet.session);
               gekkonet_process_game_events();
               g_gekkonet.paused = false; /* auto-clear; runloop never sends UNPAUSE */
            }
            else
               gekkonet_step_frame();
         }
         /* When netplay is disabled we should not stall the runloop. */
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_POST_FRAME:
         /* Post-frame polling is handled in PRE_FRAME; keep this for
          * forward compatibility. */
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_ALLOW_PAUSE:
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_ALLOW_TIMESKIP:
         ret = !g_gekkonet.running;
         break;
      case RARCH_NETPLAY_CTL_PAUSE:
         g_gekkonet.paused = true;
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_UNPAUSE:
         g_gekkonet.paused = false;
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_GAME_WATCH:
      case RARCH_NETPLAY_CTL_PLAYER_CHAT:
         ret = false;
         break;
      case RARCH_NETPLAY_CTL_LOAD_SAVESTATE:
      case RARCH_NETPLAY_CTL_RESET:
      case RARCH_NETPLAY_CTL_DESYNC_PUSH:
      case RARCH_NETPLAY_CTL_DESYNC_POP:
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_FINISHED_NAT_TRAVERSAL:
      {
         unsigned long ext_port = (unsigned long)(uintptr_t)data;
         if (ext_port)
            RARCH_LOG("[GekkoNet] NAT traversal mapped external port: %lu\n", ext_port);
         ret = true;
         break;
      }
      case RARCH_NETPLAY_CTL_REFRESH_CLIENT_INFO:
         ret = false;
         break;
      case RARCH_NETPLAY_CTL_IS_REPLAYING:
         ret = false;
         break;
      case RARCH_NETPLAY_CTL_IS_DATA_INITED:
         ret = g_gekkonet.running;
         break;
      case RARCH_NETPLAY_CTL_SET_CORE_PACKET_INTERFACE:
      case RARCH_NETPLAY_CTL_USE_CORE_PACKET_INTERFACE:
      case RARCH_NETPLAY_CTL_KICK_CLIENT:
      case RARCH_NETPLAY_CTL_BAN_CLIENT:
         ret = false;
         break;
#ifndef HAVE_DYNAMIC
      case RARCH_NETPLAY_CTL_ADD_FORK_ARG:
      case RARCH_NETPLAY_CTL_GET_FORK_ARGS:
      case RARCH_NETPLAY_CTL_CLEAR_FORK_ARGS:
         ret = false;
         break;
#endif
      default:
         ret = false;
         break;
   }

   return ret;
}
