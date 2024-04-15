#pragma once
#include "stdafx.h"

using namespace DirectX;

struct camera {
	void init(const DirectX::XMFLOAT3& position) {
		initial_position_ = position;
		reset();
	}

	void set_move_speed(float speed) {
		move_speed_ = speed;
	}

	void set_turn_speed(float speed) {
		turn_speed_ = speed;
	}

	void update(float elapsed_seconds) {
		DirectX::XMFLOAT3 move = { 0.0f, 0.0f, 0.0f };

		if (keys_pressed_.a) move.x -= 1.0f;
		if (keys_pressed_.d) move.x += 1.0f;
		if (keys_pressed_.w) move.z -= 1.0f;
		if (keys_pressed_.s) move.z += 1.0f;

		if (fabs(move.x) > 0.1f && fabs(move.z) > 0.1f) {
			auto vector = DirectX::XMVector3Normalize(XMLoadFloat3(&move));
			move.x = DirectX::XMVectorGetX(vector);
			move.z = DirectX::XMVectorGetZ(vector);
		}

		float move_interval = move_speed_ * elapsed_seconds;
		float rotate_interval = turn_speed_ * elapsed_seconds;

		if (keys_pressed_.left) z_yaw_ += rotate_interval;
		if (keys_pressed_.right) z_yaw_ -= rotate_interval;

		if (keys_pressed_.up) xz_pitch_ += rotate_interval;
		if (keys_pressed_.down) xz_pitch_ -= rotate_interval;

		// prevent looking too far up or down
		xz_pitch_ = min(xz_pitch_, DirectX::XM_PIDIV4);
		xz_pitch_ = max(-DirectX::XM_PIDIV4, xz_pitch_);

		// move the camera in model space
		float x = move.x * -cosf(z_yaw_) - move.z * sinf(z_yaw_);
		float z = move.x * sinf(z_yaw_) - move.z * cosf(z_yaw_);

		current_position_.x += x * move_interval;
		current_position_.z += z * move_interval;

		// determine the look direction
		float r = cosf(xz_pitch_);
		view_direction_.x = r * sinf(z_yaw_);
		view_direction_.y = sinf(xz_pitch_);
		view_direction_.z = r * cosf(z_yaw_);
	}

public:
	void on_keydown(WPARAM key) {
		switch (key) {
		case 'W':
			keys_pressed_.w = true;
			break;
		case 'A':
			keys_pressed_.a = true;
			break;
		case 'S':
			keys_pressed_.s = true;
			break;
		case 'D':
			keys_pressed_.d = true;
			break;
		case VK_LEFT:
			keys_pressed_.left = true;
			break;
		case VK_RIGHT:
			keys_pressed_.right = true;
			break;
		case VK_UP:
			keys_pressed_.up = true;
			break;
		case VK_DOWN:
			keys_pressed_.down = true;
			break;
		case VK_ESCAPE:
			reset();
			break;
		}
	}

	void on_keyup(WPARAM key) {
		switch (key) {
		case 'W':
			keys_pressed_.w = false;
			break;
		case 'A':
			keys_pressed_.a = false;
			break;
		case 'S':
			keys_pressed_.s = false;
			break;
		case 'D':
			keys_pressed_.d = false;
			break;
		case VK_LEFT:
			keys_pressed_.left = false;
			break;
		case VK_RIGHT:
			keys_pressed_.right = false;
			break;
		case VK_UP:
			keys_pressed_.up = false;
			break;
		case VK_DOWN:
			keys_pressed_.down = false;
			break;
		}
	}

public:
	auto view_matrix() const {
		return DirectX::XMMatrixLookToRH(DirectX::XMLoadFloat3(&current_position_), DirectX::XMLoadFloat3(&view_direction_), DirectX::XMLoadFloat3(&up_direction));
	}

	auto projection_matrix(float fov, float aspect_ratio, float near_plane, float far_plane) const {
		return DirectX::XMMatrixPerspectiveFovRH(fov, aspect_ratio, near_plane, far_plane);
	}

private:
	void reset() {
		current_position_ = initial_position_;
		z_yaw_ = DirectX::XM_PI;
		xz_pitch_ = 0.0f;
		view_direction_ = { 0.0f, 0.0f, -1.0f };
	}

private:
	struct keyboard_controller {
		bool w		= false;
		bool a		= false;
		bool s		= false;
		bool d		= false;

		bool left	= false;
		bool right	= false;
		bool up		= false;
		bool down	= false;
	};

private:
	DirectX::XMFLOAT3	initial_position_ = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3	current_position_ = initial_position_;
	DirectX::XMFLOAT3	view_direction_ = { 0.0f, 0.0f, -1.0f };
	DirectX::XMFLOAT3	up_direction = { 0.0f, 1.0f, 0.0f };

	float				z_yaw_ = DirectX::XM_PI;
	float				xz_pitch_ = 0.0f;
	float				move_speed_ = 20.0f;				// units per second
	float				turn_speed_ = DirectX::XM_PIDIV2;	// radians per second

	keyboard_controller keys_pressed_ = {};
};