/* client_main.c - Blast Arena graphical client.
 *
 * Main thread: GLFW window, keyboard, OpenGL drawing (vsync).
 * DDS thread:  started by client_start() in client_net.c.
 *
 * The main thread never waits on the network. Each frame it locks once,
 * copies the latest state, and draws it without the lock.
 *
 * Usage: ./ba_client [--name NAME] [--secure security/out/client1.xml] [-v]
 */
#include <GLFW/glfw3.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client_net.h"
#include "config.h"
#include "render.h"
#include "util.h"

/* Ctrl+C: close the window normally, so we still tell the server we left. */
static atomic_bool g_quit = false;
static void on_signal(int sig) { (void)sig; atomic_store(&g_quit, true); }

static uint8_t read_keys(GLFWwindow *w)
{
    uint8_t k = 0;
    /* Arrow keys or WASD. With two keys held, the server picks the first free
     * direction, which makes corners easy. */
    if (glfwGetKey(w, GLFW_KEY_UP) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS)
        k |= BA_KEY_UP;
    if (glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS)
        k |= BA_KEY_DOWN;
    if (glfwGetKey(w, GLFW_KEY_LEFT) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS)
        k |= BA_KEY_LEFT;
    if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS)
        k |= BA_KEY_RIGHT;
    /* Opposite keys cancel out. */
    if ((k & BA_KEY_UP) && (k & BA_KEY_DOWN))
        k &= (uint8_t)~(BA_KEY_UP | BA_KEY_DOWN);
    if ((k & BA_KEY_LEFT) && (k & BA_KEY_RIGHT))
        k &= (uint8_t)~(BA_KEY_LEFT | BA_KEY_RIGHT);
    return k;
}

static void usage(const char *prog)
{
    printf("Usage: %s [--name NAME] [--secure CONFIG.xml] [-v]\n"
           "  --name    name shown to other players (max 15 chars)\n"
           "  --secure  enable DDS Security with the given Cyclone config\n"
           "  -v        verbose debug logging\n", prog);
}

int main(int argc, char **argv)
{
    char default_name[BA_NAME_LEN];
    snprintf(default_name, sizeof default_name, "pid%d", (int)getpid() % 10000);
    ba_client_opts opts = { .name = default_name };

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--name") && i + 1 < argc)
            opts.name = argv[++i];
        else if (!strcmp(argv[i], "--secure") && i + 1 < argc)
            opts.config = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            ba_verbose = 1;
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!glfwInit())
    {
        BA_LOG("glfwInit failed (on WSL, check that WSLg / an X server is running)");
        return 1;
    }
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *win = glfwCreateWindow(BA_WIDTH, BA_HEIGHT, "Blast Arena", NULL, NULL);
    if (!win)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    /* Vsync: draw once per screen refresh, not thousands of identical frames. */
    glfwSwapInterval(1);

    ba_client *net = client_start(&opts);
    if (!net)
    {
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    render_init();
    int64_t last = ba_now_ns();
    bool bomb_was_down = false;
    ba_client_view view;

    while (!glfwWindowShouldClose(win))
    {
        glfwPollEvents();
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS || atomic_load(&g_quit))
            glfwSetWindowShouldClose(win, GLFW_TRUE);

        /* Only send keys while the window has focus. */
        bool focused = glfwGetWindowAttrib(win, GLFW_FOCUSED);
        client_set_keys(net, focused ? read_keys(win) : 0);

        /* Space / Enter drops a bomb: one bomb per press, not per frame. */
        bool bomb_down = focused && (glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS ||
                                     glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS);
        if (bomb_down && !bomb_was_down)
            client_drop_bomb(net);
        bomb_was_down = bomb_down;

        int64_t now = ba_now_ns();
        float dt = (float)(now - last) / 1e9f;
        last = now;

        /* Pixel coordinates, y down. The viewport uses the framebuffer size,
         * so it also works on HiDPI screens. */
        int fbw, fbh;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, BA_WIDTH, BA_HEIGHT, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        client_get_view(net, &view);
        render_frame(&view, now, dt);
        glfwSwapBuffers(win);
    }

    client_stop(net); /* tells the server we left */
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
