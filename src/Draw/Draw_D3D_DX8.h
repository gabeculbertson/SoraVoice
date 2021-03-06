#pragma once

#include "Draw_D3D.h"

namespace Draw {

	class D3D_DX8 : public D3D
	{
	public:
		virtual bool BeginDraw(void* pD3DD) override;
		virtual void EndDraw() override;

		virtual void DrawString(const WChar* text, int count, void* rect, unsigned format, unsigned color) override;

		virtual ~D3D_DX8();

		D3D_DX8(const char* fontName, int fontSize);

	private:
		void* dx8_data;
	};
}