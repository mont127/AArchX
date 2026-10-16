/*
 * QuickDraw's region, rectangle, point and cursor-visibility calls, which the SDK's QD headers no longer declare
 * (QD_HEADERS_ARE_PRIVATE) and ApplicationServices still exports on every architecture, arm64 included: HIToolbox
 * keeps its window and control shapes in them, and a Carbon program its clip and update areas.  These are
 * QuickDraw's documented prototypes.  Ports, GWorlds and drawing are gone from arm64; m32 provides those
 * (src/m32_qd.c).
 */
extern RgnHandle NewRgn(void);
extern void DisposeRgn(RgnHandle rgn);
extern void CopyRgn(RgnHandle srcRgn, RgnHandle dstRgn);
extern void SetEmptyRgn(RgnHandle rgn);
extern void SetRectRgn(RgnHandle rgn, short left, short top, short right, short bottom);
extern void RectRgn(RgnHandle rgn, const Rect *r);
extern void OffsetRgn(RgnHandle rgn, short dh, short dv);
extern void InsetRgn(RgnHandle rgn, short dh, short dv);
extern void SectRgn(RgnHandle srcRgnA, RgnHandle srcRgnB, RgnHandle dstRgn);
extern void UnionRgn(RgnHandle srcRgnA, RgnHandle srcRgnB, RgnHandle dstRgn);
extern void DiffRgn(RgnHandle srcRgnA, RgnHandle srcRgnB, RgnHandle dstRgn);
extern void XorRgn(RgnHandle srcRgnA, RgnHandle srcRgnB, RgnHandle dstRgn);
extern Boolean EmptyRgn(RgnHandle rgn);
extern Boolean EqualRgn(RgnHandle rgnA, RgnHandle rgnB);
extern Boolean PtInRgn(Point pt, RgnHandle rgn);
extern Boolean RectInRgn(const Rect *r, RgnHandle rgn);
extern Boolean IsRegionRectangular(RgnHandle region);
extern Rect *GetRegionBounds(RgnHandle region, Rect *bounds);
extern void MapRgn(RgnHandle rgn, const Rect *srcRect, const Rect *dstRect);
extern OSStatus QDRegionToRects(RgnHandle rgn, QDRegionParseDirection dir, RegionToRectsUPP proc, void *userData);
extern RgnHandle GetGrayRgn(void);
extern void SetRect(Rect *r, short left, short top, short right, short bottom);
extern void OffsetRect(Rect *r, short dh, short dv);
extern void InsetRect(Rect *r, short dh, short dv);
extern Boolean SectRect(const Rect *src1, const Rect *src2, Rect *dstRect);
extern void UnionRect(const Rect *src1, const Rect *src2, Rect *dstRect);
extern Boolean EqualRect(const Rect *rect1, const Rect *rect2);
extern Boolean EmptyRect(const Rect *r);
extern Boolean PtInRect(Point pt, const Rect *r);
extern void Pt2Rect(Point pt1, Point pt2, Rect *dstRect);
extern void PtToAngle(const Rect *r, Point pt, short *angle);
extern void MapRect(Rect *r, const Rect *srcRect, const Rect *dstRect);
extern void MapPt(Point *pt, const Rect *srcRect, const Rect *dstRect);
extern void AddPt(Point src, Point *dst);
extern void SubPt(Point src, Point *dst);
extern Boolean EqualPt(Point pt1, Point pt2);
extern void SetPt(Point *pt, short h, short v);
extern void InitCursor(void);
extern void HideCursor(void);
extern void ShowCursor(void);
extern void ObscureCursor(void);
extern void ShieldCursor(const Rect *shieldRect, Point offsetPt);
