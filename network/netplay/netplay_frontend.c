/* RetroArch - GekkoNet backed netplay frontend */
/* Replaces legacy RetroArch netplay stack with thin GekkoNet integration. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <boolean.h>
#include <string/stdstring.h>

#include "netplay.h"
#include "netplay_defines.h"

#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../performance_counters.h"
#include "../../input/input_driver.h"

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
   /* simple cached per-player inputs */
   uint16_t           last_buttons;
   int16_t            last_lx;
   int16_t            last_ly;
} gekko_netplay_state_t;

static net_driver_state_t networking_driver_st;
static gekko_netplay_state_t g_gekkonet;

net_driver_state_t *networking_state_get_ptr(void)
{
   return &networking_driver_st;
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

static bool gekkonet_init_session(bool is_server, const char *server, unsigned port)
{
   settings_t *settings = config_get_ptr();

   gekkonet_reset_state();

   if (!gekko_create(&g_gekkonet.session))
   {
      RARCH_ERR("[GekkoNet] Failed to create session.\n");
      return false;
   }

   g_gekkonet.adapter     = gekko_default_adapter((unsigned short)port);
   g_gekkonet.listen_port = (unsigned short)port;
   g_gekkonet.is_server   = is_server;

   if (!g_gekkonet.adapter)
   {
      RARCH_ERR("[GekkoNet] Failed to create default adapter on port %u.\n", port);
      return false;
   }

   memset(&g_gekkonet.config, 0, sizeof(g_gekkonet.config));
   g_gekkonet.config.num_players             = settings->uints.input_max_users > 1 ? 2 : 1;
   g_gekkonet.config.max_spectators          = 0;
   g_gekkonet.config.input_prediction_window = 2;
   g_gekkonet.config.spectator_delay         = 0;
   g_gekkonet.config.input_size              = sizeof(uint16_t) * 2;
   g_gekkonet.config.state_size              = (unsigned)(settings->sizes.rewind_buffer_size * 1024);
   g_gekkonet.config.limited_saving          = false;
   g_gekkonet.config.post_sync_joining       = false;
   g_gekkonet.config.desync_detection        = true;

   gekko_net_adapter_set(g_gekkonet.session, g_gekkonet.adapter);

   g_gekkonet.local_handle  = gekko_add_actor(g_gekkonet.session, LocalPlayer, NULL);

   if (is_server)
   {
      g_gekkonet.remote_handle = gekko_add_actor(g_gekkonet.session, RemotePlayer, NULL);
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

   RARCH_LOG("[GekkoNet] Netplay session started on port %u (%s).\n", port,
         is_server ? "host" : "client");

   return true;
}

static void gekkonet_shutdown(void)
{
   if (g_gekkonet.adapter || g_gekkonet.session)
      RARCH_LOG("[GekkoNet] Shutting down session.\n");

   if (g_gekkonet.session)
      gekko_destroy(g_gekkonet.session);

   gekkonet_free_remote_addr();

   gekkonet_reset_state();
}

static uint16_t gekkonet_read_buttons(void)
{
   /* Use joypad mask for player 0 */
   return (uint16_t)input_driver_state_wrapper(0, RETRO_DEVICE_JOYPAD,
            0, RETRO_DEVICE_ID_JOYPAD_MASK);
}

static void gekkonet_push_local_input(void)
{
   uint16_t buttons = gekkonet_read_buttons();
   int16_t lx       = input_driver_state_wrapper(0, RETRO_DEVICE_ANALOG,
         RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
   int16_t ly       = input_driver_state_wrapper(0, RETRO_DEVICE_ANALOG,
         RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

   /* Pack buttons + simple analog into buffer */
   uint16_t payload[2];
   payload[0] = buttons;
   payload[1] = (uint16_t)(((lx & 0xFF) << 8) | (ly & 0xFF));

   g_gekkonet.last_buttons = buttons;
   g_gekkonet.last_lx      = lx;
   g_gekkonet.last_ly      = ly;

   gekko_add_local_input(g_gekkonet.session, g_gekkonet.local_handle, payload);
}

static void gekkonet_poll(void)
{
   gekko_network_poll(g_gekkonet.session);

   /* drain events to keep session progressing */
   int event_count          = 0;
   GekkoGameEvent **events  = gekko_update_session(g_gekkonet.session, &event_count);

   for (int i = 0; i < event_count; i++)
   {
      GekkoGameEvent *evt = events[i];
      switch (evt->type)
      {
         case AdvanceEvent:
            /* Inputs are processed internally by GekkoNet; RetroArch already polled
             * local inputs, so we just continue. */
            break;
         case SaveEvent:
         case LoadEvent:
         default:
            break;
      }
   }
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
      char *address, unsigned *port, char *session, size_t len)
{
   if (!hostname || !address || !port)
      return false;

   const char *colon = strchr(hostname, ':');
   if (colon)
   {
      strlcpy(address, hostname, (size_t)(colon - hostname + 1));
      *port = (unsigned)strtoul(colon + 1, NULL, 10);
   }
   else
   {
      strlcpy(address, hostname, len);
      *port = 55435; /* default */
   }

   if (session)
      strlcpy(session, "", len);

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

   if (g_gekkonet.running)
      return true;

   networking_driver_st.data = (netplay_t*)&g_gekkonet;

   return gekkonet_init_session(server == NULL || string_is_empty(server), server, port);
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
            gekkonet_push_local_input();
            ret = true;
         }
         else
            ret = false;
         break;
      case RARCH_NETPLAY_CTL_POST_FRAME:
         if (g_gekkonet.running)
            gekkonet_poll();
         break;
      case RARCH_NETPLAY_CTL_ALLOW_PAUSE:
      case RARCH_NETPLAY_CTL_ALLOW_TIMESKIP:
         ret = true;
         break;
      case RARCH_NETPLAY_CTL_PAUSE:
      case RARCH_NETPLAY_CTL_UNPAUSE:
      case RARCH_NETPLAY_CTL_GAME_WATCH:
      case RARCH_NETPLAY_CTL_PLAYER_CHAT:
      case RARCH_NETPLAY_CTL_LOAD_SAVESTATE:
      case RARCH_NETPLAY_CTL_RESET:
      case RARCH_NETPLAY_CTL_FINISHED_NAT_TRAVERSAL:
      case RARCH_NETPLAY_CTL_DESYNC_PUSH:
      case RARCH_NETPLAY_CTL_DESYNC_POP:
      case RARCH_NETPLAY_CTL_REFRESH_CLIENT_INFO:
      case RARCH_NETPLAY_CTL_IS_REPLAYING:
      case RARCH_NETPLAY_CTL_IS_DATA_INITED:
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
