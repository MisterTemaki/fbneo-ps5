// license:BSD-3-Clause
// copyright-holders:Ville Linde, Aaron Giles
//
// FBNeo PS5 2.3: what MAME's src/devices/video/poly.h (mamedev/mame b7f483f) gives Sega Model 2's renderer, without
// the threads: render_triangle and render_polygon compute each scanline's extent and parameters exactly as poly.h
// does (same rounding, same pixel centres, same left/right edge walk) and call the scanline callback at once, in the
// order the primitives come (poly.h's work queue keeps that order on each scanline too, so the front-to-back fill of
// the Model 2 renderer gives the same pixels).
#ifndef FBNEO_MODEL2_POLY_H
#define FBNEO_MODEL2_POLY_H

#include <stdint.h>
#include <math.h>
#include <algorithm>
#include <limits>

// MAME's rectangle (inclusive bounds)
struct m2_rectangle
{
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	m2_rectangle() { }
	m2_rectangle(int x0, int x1, int y0, int y1) : min_x(x0), max_x(x1), min_y(y0), max_y(y1) { }
	int left() const { return min_x; }
	int right() const { return max_x; }
	int top() const { return min_y; }
	int bottom() const { return max_y; }
	m2_rectangle& operator&=(const m2_rectangle& r)
	{
		if (r.min_x > min_x) min_x = r.min_x;
		if (r.max_x < max_x) max_x = r.max_x;
		if (r.min_y > min_y) min_y = r.min_y;
		if (r.max_y < max_y) max_y = r.max_y;
		return *this;
	}
};

template<typename BaseType, class ObjectType, int MaxParams>
class m2_poly_manager
{
public:
	struct vertex_t
	{
		vertex_t() { }
		vertex_t(BaseType _x, BaseType _y) { x = _x; y = _y; }

		BaseType x, y;            // X, Y coordinates
		BaseType p[MaxParams];    // iterated parameters
	};

	struct extent_t
	{
		struct param_t
		{
			BaseType start;       // parameter value at start
			BaseType dpdx;        // dp/dx relative to start
		};
		int16_t startx, stopx;    // starting (inclusive)/ending (exclusive) endpoints
		param_t param[MaxParams]; // array of parameter start/delays
	};

	// the scanline callback (MAME's render_delegate)
	typedef void (*render_func)(void* owner, int32_t scanline, const extent_t& extent, const ObjectType& object, int threadid);
	struct render_delegate
	{
		render_func func;
		void* owner;
	};

	// one object at a time: it is rendered at once
	struct object_array
	{
		ObjectType obj;
		ObjectType& next() { return obj; }
	};
	object_array& object_data() { return m_object; }

	void wait(const char* = "general") { }

	static int32_t round_coordinate(BaseType value)
	{
		// saturate (avoid overflow/underflow)
		if (value >= BaseType(std::numeric_limits<int32_t>::max()))
			return std::numeric_limits<int32_t>::max();
		const BaseType ipart = std::floor(value);
		if (ipart < BaseType(std::numeric_limits<int32_t>::min()))
			return std::numeric_limits<int32_t>::min();
		// round (MAME: the midpoint goes towards negative infinity)
		const BaseType fpart = value - ipart;
		return int32_t(ipart) + ((fpart > BaseType(0.5)) ? 1 : 0);
	}

	// poly.h's render_triangle, the extents rendered as they are made
	template<int ParamCount>
	uint32_t render_triangle(const m2_rectangle& cliprect, const render_delegate& callback, const vertex_t& _v1, const vertex_t& _v2, const vertex_t& _v3)
	{
		const vertex_t* v1 = &_v1;
		const vertex_t* v2 = &_v2;
		const vertex_t* v3 = &_v3;

		// first sort by Y
		if (v2->y < v1->y)
			std::swap(v1, v2);
		if (v3->y < v2->y)
		{
			std::swap(v2, v3);
			if (v2->y < v1->y)
				std::swap(v1, v2);
		}

		// compute some integral X/Y vertex values
		int32_t v1y = round_coordinate(v1->y);
		int32_t v3y = round_coordinate(v3->y);

		// clip coordinates
		int32_t v1yclip = std::max(v1y, cliprect.top());
		int32_t v3yclip = std::min(v3y, cliprect.bottom() + 1);
		if (v3yclip - v1yclip <= 0)
			return 0;

		// compute the slopes for each portion of the triangle
		BaseType dxdy_v1v2 = (v2->y == v1->y) ? BaseType(0.0) : (v2->x - v1->x) / (v2->y - v1->y);
		BaseType dxdy_v1v3 = (v3->y == v1->y) ? BaseType(0.0) : (v3->x - v1->x) / (v3->y - v1->y);
		BaseType dxdy_v2v3 = (v3->y == v2->y) ? BaseType(0.0) : (v3->x - v2->x) / (v3->y - v2->y);

		// compute parameter starting points and deltas
		BaseType param_start[MaxParams > 0 ? MaxParams : 1];
		BaseType param_dpdx[MaxParams > 0 ? MaxParams : 1];
		BaseType param_dpdy[MaxParams > 0 ? MaxParams : 1];
		if (ParamCount > 0)
		{
			BaseType a00 = v2->y - v3->y;
			BaseType a01 = v3->x - v2->x;
			BaseType a02 = v2->x*v3->y - v3->x*v2->y;
			BaseType a10 = v3->y - v1->y;
			BaseType a11 = v1->x - v3->x;
			BaseType a12 = v3->x*v1->y - v1->x*v3->y;
			BaseType a20 = v1->y - v2->y;
			BaseType a21 = v2->x - v1->x;
			BaseType a22 = v1->x*v2->y - v2->x*v1->y;
			BaseType det = a02 + a12 + a22;

			if (std::abs(det) < BaseType(0.00001))
			{
				for (int paramnum = 0; paramnum < ParamCount; paramnum++)
				{
					param_dpdx[paramnum] = BaseType(0.0);
					param_dpdy[paramnum] = BaseType(0.0);
					param_start[paramnum] = v1->p[paramnum];
				}
			}
			else
			{
				BaseType idet = BaseType(1.0) / det;
				for (int paramnum = 0; paramnum < ParamCount; paramnum++)
				{
					param_dpdx[paramnum]  = idet * (v1->p[paramnum]*a00 + v2->p[paramnum]*a10 + v3->p[paramnum]*a20);
					param_dpdy[paramnum]  = idet * (v1->p[paramnum]*a01 + v2->p[paramnum]*a11 + v3->p[paramnum]*a21);
					param_start[paramnum] = idet * (v1->p[paramnum]*a02 + v2->p[paramnum]*a12 + v3->p[paramnum]*a22);
				}
			}
		}

		// compute the X extents for each scanline
		uint32_t pixels = 0;
		extent_t extent;
		for (int32_t curscan = v1yclip; curscan < v3yclip; curscan++)
		{
			// compute the ending X based on which part of the triangle we're in
			BaseType fully = BaseType(curscan) + BaseType(0.5);
			BaseType startx = v1->x + (fully - v1->y) * dxdy_v1v3;
			BaseType stopx;
			if (fully < v2->y)
				stopx = v1->x + (fully - v1->y) * dxdy_v1v2;
			else
				stopx = v2->x + (fully - v2->y) * dxdy_v2v3;

			// clamp to full pixels
			int32_t istartx = round_coordinate(startx);
			int32_t istopx = round_coordinate(stopx);

			// force start < stop
			if (istartx > istopx)
				std::swap(istartx, istopx);

			// apply left/right clipping
			istartx = std::max(istartx, cliprect.left());
			istopx = std::min(istopx, cliprect.right() + 1);

			// set the extent and update the total pixel count
			if (istartx >= istopx)
				istartx = istopx = 0;
			extent.startx = istartx;
			extent.stopx = istopx;
			pixels += istopx - istartx;

			// fill in the parameters for the extent
			BaseType fullstartx = BaseType(istartx) + BaseType(0.5);
			for (int paramnum = 0; paramnum < ParamCount; paramnum++)
			{
				extent.param[paramnum].start = param_start[paramnum] + fullstartx * param_dpdx[paramnum] + fully * param_dpdy[paramnum];
				extent.param[paramnum].dpdx = param_dpdx[paramnum];
			}
			if (istartx < istopx)
				callback.func(callback.owner, curscan, extent, m_object.obj, 0);
		}
		return pixels;
	}

	// poly.h's render_polygon, the extents rendered as they are made
	template<int NumVerts, int ParamCount>
	uint32_t render_polygon(const m2_rectangle& cliprect, const render_delegate& callback, const vertex_t* v)
	{
		// determine min/max Y vertices
		BaseType minx = v[0].x;
		BaseType maxx = v[0].x;
		int minv = 0;
		int maxv = 0;
		for (int vertnum = 1; vertnum < NumVerts; vertnum++)
		{
			if (v[vertnum].y < v[minv].y)
				minv = vertnum;
			else if (v[vertnum].y > v[maxv].y)
				maxv = vertnum;
			minx = std::min(minx, v[vertnum].x);
			maxx = std::max(maxx, v[vertnum].x);
		}
		(void)minx; (void)maxx;

		// determine start/end scanlines
		int32_t miny = round_coordinate(v[minv].y);
		int32_t maxy = round_coordinate(v[maxv].y);

		// clip coordinates
		int32_t minyclip = std::max(miny, cliprect.top());
		int32_t maxyclip = std::min(maxy, cliprect.bottom() + 1);
		if (maxyclip - minyclip <= 0)
			return 0;

		struct poly_edge
		{
			const vertex_t* v1;           // pointer to first vertex
			const vertex_t* v2;           // pointer to second vertex
			BaseType dxdy;                // dx/dy along the edge
			BaseType dpdy[MaxParams];     // per-parameter dp/dy values
		};
		// walk forward to build up the forward edge list
		poly_edge fedgelist[NumVerts - 1];
		poly_edge* edgeptr = &fedgelist[0];
		for (int curv = minv; curv != maxv; curv = (curv == NumVerts - 1) ? 0 : (curv + 1))
		{
			// set the two vertices
			edgeptr->v1 = &v[curv];
			edgeptr->v2 = &v[(curv == NumVerts - 1) ? 0 : (curv + 1)];

			// if horizontal, skip altogether
			if (edgeptr->v1->y == edgeptr->v2->y)
				continue;

			// need dx/dy always, and parameter deltas as necessary
			BaseType ooy = BaseType(1.0) / (edgeptr->v2->y - edgeptr->v1->y);
			edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
			for (int paramnum = 0; paramnum < ParamCount; paramnum++)
				edgeptr->dpdy[paramnum] = (edgeptr->v2->p[paramnum] - edgeptr->v1->p[paramnum]) * ooy;
			++edgeptr;
		}

		// walk backward to build up the backward edge list
		poly_edge bedgelist[NumVerts - 1];
		edgeptr = &bedgelist[0];
		for (int curv = minv; curv != maxv; curv = (curv == 0) ? (NumVerts - 1) : (curv - 1))
		{
			// set the two vertices
			edgeptr->v1 = &v[curv];
			edgeptr->v2 = &v[(curv == 0) ? (NumVerts - 1) : (curv - 1)];

			// if horizontal, skip altogether
			if (edgeptr->v1->y == edgeptr->v2->y)
				continue;

			// need dx/dy always, and parameter deltas as necessary
			BaseType ooy = BaseType(1.0) / (edgeptr->v2->y - edgeptr->v1->y);
			edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
			for (int paramnum = 0; paramnum < ParamCount; paramnum++)
				edgeptr->dpdy[paramnum] = (edgeptr->v2->p[paramnum] - edgeptr->v1->p[paramnum]) * ooy;
			++edgeptr;
		}

		// determine which list is left/right:
		// if the first vertex is shared, compare the slopes
		// if the first vertex is not shared, compare the X coordinates
		const poly_edge *ledge, *redge;
		if ((fedgelist[0].v1 == bedgelist[0].v1 && fedgelist[0].dxdy < bedgelist[0].dxdy) ||
			(fedgelist[0].v1 != bedgelist[0].v1 && fedgelist[0].v1->x < bedgelist[0].v1->x))
		{
			ledge = fedgelist;
			redge = bedgelist;
		}
		else
		{
			ledge = bedgelist;
			redge = fedgelist;
		}

		// compute the X extents for each scanline
		uint32_t pixels = 0;
		extent_t extent;
		for (int32_t curscan = minyclip; curscan < maxyclip; curscan++)
		{
			// compute the ending X based on which part of the triangle we're in
			BaseType fully = BaseType(curscan) + BaseType(0.5);
			while (fully > ledge->v2->y && fully < v[maxv].y)
				++ledge;
			while (fully > redge->v2->y && fully < v[maxv].y)
				++redge;
			BaseType startx = ledge->v1->x + (fully - ledge->v1->y) * ledge->dxdy;
			BaseType stopx = redge->v1->x + (fully - redge->v1->y) * redge->dxdy;

			// clamp to full pixels
			int32_t istartx = round_coordinate(startx);
			int32_t istopx = round_coordinate(stopx);

			// force start < stop
			if (istartx > istopx)
				std::swap(istartx, istopx);

			// apply left/right clipping BEFORE calculating parameter start
			istartx = std::max(istartx, cliprect.left());
			istopx = std::min(istopx, cliprect.right() + 1);

			if (ParamCount > 0)
			{
				BaseType ldy = fully - ledge->v1->y;
				BaseType rdy = fully - redge->v1->y;
				BaseType oox = BaseType(1.0) / (stopx - startx);

				// iterate over parameters
				for (int paramnum = 0; paramnum < ParamCount; paramnum++)
				{
					BaseType lparam = ledge->v1->p[paramnum] + ldy * ledge->dpdy[paramnum];
					BaseType rparam = redge->v1->p[paramnum] + rdy * redge->dpdy[paramnum];

					BaseType dpdx = (rparam - lparam) * oox;

					extent.param[paramnum].start = lparam + (BaseType(istartx) + BaseType(0.5) - startx) * dpdx;
					extent.param[paramnum].dpdx = dpdx;
				}
			}

			// set the extent and update the total pixel count
			if (istartx >= istopx)
				istartx = istopx = 0;
			extent.startx = istartx;
			extent.stopx = istopx;
			pixels += istopx - istartx;
			if (istartx < istopx)
				callback.func(callback.owner, curscan, extent, m_object.obj, 0);
		}
		return pixels;
	}

protected:
	object_array m_object;
};

#endif
