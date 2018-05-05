#pragma once
#include <optional>
#include <vector>
#include <array>

#include "pack_structs.h"

namespace rectpack {
	class node;
	using node_array_type = std::array<node, 10000>;

	class node {
		static int nodes_size;
		static node_array_type all_nodes;

		class child_node {
			int ptr = -1;
		public:

			bool is_allocated() const {
				return ptr != -1;
			}

			auto& get() {
				return all_nodes[ptr];
			}

			const auto& get() const {
				return all_nodes[ptr];
			}

			void set(const rect_ltrb& r) {
				if (ptr == -1) { 
					ptr = nodes_size++;
				}

				all_nodes[ptr] = r;
			}
		};

		void grow_branch(rect_xywhf& img) {
			const auto iw = img.flipped ? img.h : img.w;
			const auto ih = img.flipped ? img.w : img.h;

			if (rc.w() - iw > rc.h() - ih) {
				child[0].set({ rc.l, rc.t, rc.l + iw, rc.b });
				child[1].set({ rc.l + iw, rc.t, rc.r, rc.b });
			}
			else {
				child[0].set({ rc.l, rc.t, rc.r, rc.t + ih });
				child[1].set({ rc.l, rc.t + ih, rc.r, rc.b });
			}
		}

		rect_ltrb rc;
		child_node child[2];
		bool leaf_filled = false;

		enum class leaf_fill {
			TOO_BIG,
			EXACT,
			GROW
		};

	public:
		bool is_empty_leaf() const {
			return !child[0].is_allocated() && !child[1].is_allocated() && !leaf_filled;
		}

		static auto make_root(const rect_wh& r) {
			nodes_size = 0;
			return node({0, 0, r.w, r.h});
		};

		node(rect_ltrb rc = rect_ltrb()) : rc(rc) {}

		auto get_rc() const {
			return rc;
		}

		leaf_fill get_filling(rect_xywhf& img, const bool allow_flip) const {
			switch (img.get_fitting(rect_xywh(rc), allow_flip)) {
				case rect_wh_fitting::TOO_BIG: 
					return leaf_fill::TOO_BIG;

				case rect_wh_fitting::FITS_INSIDE:
					img.flipped = false; 
					return leaf_fill::GROW;

				case rect_wh_fitting::FITS_INSIDE_BUT_FLIPPED: 	
					img.flipped = true; 
					return leaf_fill::GROW;

				case rect_wh_fitting::FITS_EXACTLY:
					img.flipped = false; 
					return leaf_fill::EXACT;

				case rect_wh_fitting::FITS_EXACTLY_BUT_FLIPPED: 
					img.flipped = true;  
					return leaf_fill::EXACT;
			}
		}

		node* leaf_insert(rect_xywhf& img, const bool allow_flip) {
			const auto result = get_filling(img, allow_flip);

			if (result == leaf_fill::EXACT) {
				leaf_filled = true; 
				return this;
			}
			else if (result == leaf_fill::GROW) {
				grow_branch(img);

				auto& new_leaf = child[0].get();
				const auto second_result = new_leaf.get_filling(img, allow_flip);

				if (second_result == leaf_fill::GROW) {
					new_leaf.grow_branch(img);

					/* Left leaf of the new child must fit exactly by this point. */
					auto& target_leaf = new_leaf.child[0].get();
					target_leaf.leaf_filled = true;
					return &target_leaf;
				}
				else if (second_result == leaf_fill::EXACT) {
					new_leaf.leaf_filled = true;
					return &new_leaf;
				}
				else {
					/* 
						Should never happen. 
						It could have not been too big by this point. 
					*/
				}
			}

			/* Control may only reach here when the img was too big. */
			return nullptr;
		}

		node* insert(rect_xywhf& img, const bool allow_flip) {
			if (child[0].is_allocated()) {
				/* This is a branch. */

				if (const auto inserted_left = child[0].get().insert(img, allow_flip)) {
					return inserted_left;
				}

				/* Insert to the right otherwise */
				return child[1].get().insert(img, allow_flip);
			}

			/* This is a leaf. */

			if (leaf_filled) {
				return nullptr;
			}

			return leaf_insert(img, allow_flip);
		}

		template <class T>
		void readback(
			rect_xywhf& into,
			T& tracked_dimensions
		) const {
			into.x = rc.l;
			into.y = rc.t;

			if (into.flipped) {
				std::swap(into.w, into.h);
			}

			tracked_dimensions.x = std::max(tracked_dimensions.x, rc.r);
			tracked_dimensions.y = std::max(tracked_dimensions.y, rc.b); 
		}
	};

	template <class F, class G, class... Comparators>
	rect_wh pack_rectangles(
		const std::vector<rect_xywhf*>& input, 
		const int max_bin_side, 
		const bool allow_flip, 
		F push_successful,
		G push_unsuccessful,
		const int discard_step,
		Comparators... comparators
	) {
		constexpr auto funcs = sizeof...(Comparators);
		const auto n = input.size();

		thread_local std::vector<rect_xywhf*> order[funcs];

		bool (*cmpf[funcs])(rect_xywhf*, rect_xywhf*) = {
			comparators...
		};

		for (std::size_t f = 0; f < funcs; ++f) { 
			order[f] = input;
			std::sort(order[f].begin(), order[f].end(), cmpf[f]);
		}

		rect_wh min_bin = rect_wh(max_bin_side, max_bin_side);

		std::optional<int> min_func;

		int	best_func = 0;
		int best_area = 0;
		int current_area = 0;

		bool fail = false;

		for (unsigned f = 0; f < funcs; ++f) {
			const auto& v = order[f];

			int step = min_bin.w / 2;
			auto root = node::make_root(min_bin);

			while (true) {
				if (root.get_rc().w() > min_bin.w) {
					/* 
						If we are now going to attempt packing into a bin
						that is bigger than the current minimum, abort.
					*/

					if (min_func) {
						break;
					}

					current_area = 0;

					root = node::make_root(min_bin);

					for (std::size_t i = 0; i < n; ++i) {
						if (root.insert(*v[i], allow_flip)) {
							current_area += v[i]->area();
						}
					}

					fail = true;
					break;
				}

				const bool all_inserted = [&]() {
					for (std::size_t i = 0; i < n; ++i) {
						if (!root.insert(*v[i], allow_flip)) {
							return false;
						}
					}

					return true;
				}();

				if (all_inserted) {
					if (step <= discard_step) {
						break;
					}

					/* Attempt was successful. Try with a smaller bin. */
					root = node::make_root({ root.get_rc().w() - step, root.get_rc().h() - step });
				}
				else {
					/* Attempt ended in failure. Try with a bigger bin. */
					root = node::make_root({ root.get_rc().w() + step, root.get_rc().h() + step });
				}

				step /= 2;

				if (!step) {
					step = 1;
				}
			}

			if (!fail && (min_bin.area() >= root.get_rc().area())) {
				min_bin = rect_wh(root.get_rc());
				min_func = f;
			}
			else if (fail && (current_area > best_area)) {
				best_area = current_area;
				best_func = f;
			}

			fail = false;
		}

		struct {
			int x = 0;
			int y = 0;
		} clip;

		{
			auto& v = order[min_func ? *min_func : best_func];

			auto root = node::make_root(min_bin);

			for (std::size_t i = 0; i < n; ++i) {
				if (const auto ret = root.insert(*v[i],allow_flip)) {
					ret->readback(*v[i], clip);
					push_successful(v[i]);
				}
				else {
					push_unsuccessful(v[i]);

					v[i]->flipped = false;
				}
			}
		}

		return rect_wh(clip.x, clip.y);
	}

	template <class F, class G>
	rect_wh pack_rectangles(
		const std::vector<rect_xywhf*>& input, 
		const int max_bin_side, 
		const bool allow_flip, 
		F push_successful,
		G push_unsuccessful,
		const int discard_step
	) {
		auto area = [](rect_xywhf* a, rect_xywhf* b) {
			return a->area() > b->area();
		};

		auto perimeter = [](rect_xywhf* a, rect_xywhf* b) {
			return a->perimeter() > b->perimeter();
		};

		auto max_side = [](rect_xywhf* a, rect_xywhf* b) {
			return std::max(a->w, a->h) > std::max(b->w, b->h);
		};

		auto max_width = [](rect_xywhf* a, rect_xywhf* b) {
			return a->w > b->w;
		};

		auto max_height = [](rect_xywhf* a, rect_xywhf* b) {
			return a->h > b->h;
		};

		return pack_rectangles(
			input,

			max_bin_side,
			allow_flip,
			push_successful,
			push_unsuccessful,
			discard_step,

			area,
			perimeter,
			max_side,
			max_width,
			max_height
		);
	}
}

rectpack::node_array_type rectpack::node::all_nodes;
int rectpack::node::nodes_size = 0;
