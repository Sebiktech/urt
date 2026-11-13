#include "debug_view.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/components.hpp"

#include <cmath>

// GLFW's header also pulls in the system OpenGL header.
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

namespace {

void on_glfw_error(int code, const char* description) {
    urt::log::error("window", "GLFW error {}: {}", code, description);
}

// A box standing on the ground: centred on x and z, from y = 0 up to height.
// Each side gets its own brightness; with no lighting that is what makes the
// shape readable.
void draw_box(float half_width, float height, float r, float g, float b) {
    const float w = half_width;
    const float h = height;
    glBegin(GL_QUADS);
    glColor3f(r, g, b); // top
    glVertex3f(-w, h, -w); glVertex3f(-w, h, w); glVertex3f(w, h, w); glVertex3f(w, h, -w);
    glColor3f(r * 0.8f, g * 0.8f, b * 0.8f); // +z (faces the camera)
    glVertex3f(-w, 0, w); glVertex3f(w, 0, w); glVertex3f(w, h, w); glVertex3f(-w, h, w);
    glColor3f(r * 0.5f, g * 0.5f, b * 0.5f); // -z
    glVertex3f(-w, 0, -w); glVertex3f(-w, h, -w); glVertex3f(w, h, -w); glVertex3f(w, 0, -w);
    glColor3f(r * 0.65f, g * 0.65f, b * 0.65f); // +x and -x
    glVertex3f(w, 0, -w); glVertex3f(w, h, -w); glVertex3f(w, h, w); glVertex3f(w, 0, w);
    glVertex3f(-w, 0, -w); glVertex3f(-w, 0, w); glVertex3f(-w, h, w); glVertex3f(-w, h, -w);
    glEnd();
}

void draw_grid(const glm::vec3& focus) {
    constexpr float spacing = 2.0f;
    constexpr int lines = 30; // each side of the focus point
    // Snap to the spacing so the grid stays put while the camera moves.
    const float cx = std::floor(focus.x / spacing) * spacing;
    const float cz = std::floor(focus.z / spacing) * spacing;
    const float extent = spacing * static_cast<float>(lines);

    glColor3f(0.22f, 0.24f, 0.28f);
    glBegin(GL_LINES);
    for (int i = -lines; i <= lines; ++i) {
        const float offset = static_cast<float>(i) * spacing;
        glVertex3f(cx + offset, 0, cz - extent); glVertex3f(cx + offset, 0, cz + extent);
        glVertex3f(cx - extent, 0, cz + offset); glVertex3f(cx + extent, 0, cz + offset);
    }
    glEnd();
}

} // namespace

DebugView::~DebugView() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

bool DebugView::open(int width, int height, const char* title) {
    glfwSetErrorCallback(on_glfw_error);
    if (glfwInit() == GLFW_FALSE) {
        return false;
    }
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // wait for the monitor: no tearing, no 100 % CPU
    return true;
}

bool DebugView::should_close() const {
    return window_ == nullptr || glfwWindowShouldClose(window_) == GLFW_TRUE;
}

urt::MoveKeys DebugView::poll_keys() {
    glfwPollEvents();
    const auto down = [this](int key) { return glfwGetKey(window_, key) == GLFW_PRESS; };
    if (down(GLFW_KEY_ESCAPE)) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    urt::MoveKeys keys;
    keys.forward = down(GLFW_KEY_W) || down(GLFW_KEY_UP);
    keys.back = down(GLFW_KEY_S) || down(GLFW_KEY_DOWN);
    keys.left = down(GLFW_KEY_A) || down(GLFW_KEY_LEFT);
    keys.right = down(GLFW_KEY_D) || down(GLFW_KEY_RIGHT);
    return keys;
}

void DebugView::set_title(const std::string& title) { glfwSetWindowTitle(window_, title.c_str()); }

void DebugView::draw(urt::World& world, urt::Entity local_player) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width <= 0 || height <= 0) {
        return; // minimised
    }

    auto& registry = world.registry;
    glm::vec3 focus{0.0f};
    if (local_player != entt::null && registry.valid(local_player)) {
        focus = registry.get<urt::Transform>(local_player).position;
    }

    glViewport(0, 0, width, height);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // The camera sits above and behind the player and looks down at them.
    // "Behind" is +Z, so up on the screen is -Z: that is where W walks.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::mat4 projection = glm::perspective(glm::radians(50.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::lookAt(focus + glm::vec3{0.0f, 14.0f, 12.0f}, focus, glm::vec3{0.0f, 1.0f, 0.0f});
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(glm::value_ptr(projection));
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(glm::value_ptr(view));

    draw_grid(focus);

    for (auto [entity, id, transform] :
         registry.view<const urt::net::NetworkId, const urt::Transform>().each()) {
        glPushMatrix();
        glTranslatef(transform.position.x, transform.position.y, transform.position.z);
        // Forward is (cos yaw, sin yaw) on XZ. OpenGL turns +X towards -Z for
        // a positive angle around Y, hence the minus.
        glRotatef(-glm::degrees(transform.yaw), 0.0f, 1.0f, 0.0f);

        if (entity == local_player) {
            draw_box(0.3f, 1.8f, 1.0f, 0.6f, 0.2f); // us: orange
        } else {
            draw_box(0.3f, 1.8f, 0.3f, 0.6f, 1.0f); // everyone else: blue
        }
        // A small "nose" at head height showing which way the entity faces.
        glTranslatef(0.45f, 1.4f, 0.0f);
        draw_box(0.15f, 0.2f, 0.95f, 0.95f, 0.95f);
        glPopMatrix();
    }

    glfwSwapBuffers(window_);
}
