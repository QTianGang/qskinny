/******************************************************************************
 * QSkinny - Copyright (C) 2016 Uwe Rathmann
 * This file may be used under the terms of the QSkinny License, Version 1.0
 *****************************************************************************/

#include "QskTextureRenderer.h"
#include "QskColorFilter.h"
#include "QskGraphic.h"
#include "QskSetup.h"

#include <qopenglcontext.h>
#include <qopenglextrafunctions.h>
#include <qopenglframebufferobject.h>
#include <qopenglfunctions.h>
#include <qopenglpaintdevice.h>
#include <qopengltexture.h>

#include <qimage.h>
#include <qpainter.h>

#include <qguiapplication.h>
#include <qquickwindow.h>
#include <qscreen.h>
#include <qsurface.h>

static uint qskCreateTextureOpenGL(
    const QSize& size, QskTextureRenderer::PaintHelper* helper )
{
    const int width = size.width();
    const int height = size.height();

    QOpenGLFramebufferObjectFormat format1;
    format1.setAttachment( QOpenGLFramebufferObject::CombinedDepthStencil );

    // ### TODO: get samples from window instead
    format1.setSamples( QOpenGLContext::currentContext()->format().samples() );

    QOpenGLFramebufferObject multisampledFbo( width, height, format1 );

    QOpenGLPaintDevice pd( width, height );
    pd.setPaintFlipped( true );

    {
        QPainter painter( &pd );

        painter.setCompositionMode( QPainter::CompositionMode_Source );
        painter.fillRect( 0, 0, width, height, Qt::transparent );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );

        helper->paint( &painter, size );

#if 1
        if ( format1.samples() > 0 )
        {
            /*
                Multisampling in the window surface might get lost
                as a side effect of rendering to the FBO.
                weired, needs to be investigated more
             */
            painter.setRenderHint( QPainter::Antialiasing, true );
        }
#endif
    }

    QOpenGLFramebufferObjectFormat format2;
    format2.setAttachment( QOpenGLFramebufferObject::NoAttachment );

    QOpenGLFramebufferObject fbo( width, height, format2 );

    const QRect fboRect( 0, 0, width, height );

    QOpenGLFramebufferObject::blitFramebuffer(
        &fbo, fboRect, &multisampledFbo, fboRect );

    return fbo.takeTexture();
}

static uint qskCreateTextureRaster(
    const QSize& size, QskTextureRenderer::PaintHelper* helper )
{
    QImage image( size, QImage::Format_RGBA8888_Premultiplied );
    image.fill( Qt::transparent );

    {
        QPainter painter( &image );
        helper->paint( &painter, size );
    }

    const auto target = QOpenGLTexture::Target2D;

    auto context = QOpenGLContext::currentContext();
    if ( context == nullptr )
        return 0;

    auto& f = *context->functions();

    GLint oldTexture; // we can't rely on having OpenGL Direct State Access
    f.glGetIntegerv( QOpenGLTexture::BindingTarget2D, &oldTexture );

    GLuint textureId;
    f.glGenTextures( 1, &textureId );

    f.glBindTexture( target, textureId );

    f.glTexParameteri( target, GL_TEXTURE_MIN_FILTER, QOpenGLTexture::Nearest );
    f.glTexParameteri( target, GL_TEXTURE_MAG_FILTER, QOpenGLTexture::Nearest );

    f.glTexParameteri( target, GL_TEXTURE_WRAP_S, QOpenGLTexture::ClampToEdge );
    f.glTexParameteri( target, GL_TEXTURE_WRAP_T, QOpenGLTexture::ClampToEdge );

    if ( QOpenGLTexture::hasFeature( QOpenGLTexture::ImmutableStorage ) )
    {
        auto& ef = *context->extraFunctions();
        ef.glTexStorage2D( target, 1,
            QOpenGLTexture::RGBA8_UNorm, image.width(), image.height() );

        f.glTexSubImage2D( target, 0, 0, 0, image.width(), image.height(),
            QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, image.constBits() );
    }
    else
    {
        f.glTexImage2D( target, 0, QOpenGLTexture::RGBA8_UNorm,
            image.width(), image.height(), 0,
            QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, image.constBits() );
    }

    f.glBindTexture( target, oldTexture );

    return textureId;
}

QskTextureRenderer::PaintHelper::~PaintHelper()
{
}

uint QskTextureRenderer::createTexture(
    RenderMode renderMode, const QSize& size, PaintHelper* helper )
{
    if ( renderMode == AutoDetect )
    {
        if ( qskSetup->controlFlags() & QskSetup::PreferRasterForTextures )
            renderMode = Raster;
        else
            renderMode = OpenGL;
    }

    if ( renderMode == Raster )
        return qskCreateTextureRaster( size, helper );
    else
        return qskCreateTextureOpenGL( size, helper );
}

uint QskTextureRenderer::createTextureFromGraphic(
    RenderMode renderMode, const QSize& size,
    const QskGraphic& graphic, const QskColorFilter& colorFilter,
    Qt::AspectRatioMode aspectRatioMode )
{
    class PaintHelper : public QskTextureRenderer::PaintHelper
    {
      public:
        PaintHelper( const QskGraphic& graphic,
                const QskColorFilter& filter, Qt::AspectRatioMode aspectRatioMode )
            : m_graphic( graphic )
            , m_filter( filter )
            , m_aspectRatioMode( aspectRatioMode )
        {
        }

        void paint( QPainter* painter, const QSize& size ) override
        {
            const QRect rect( 0, 0, size.width(), size.height() );
            m_graphic.render( painter, rect, m_filter, m_aspectRatioMode );
        }

      private:
        const QskGraphic& m_graphic;
        const QskColorFilter& m_filter;
        const Qt::AspectRatioMode m_aspectRatioMode;
    };

    PaintHelper helper( graphic, colorFilter, aspectRatioMode );
    return createTexture( renderMode, size, &helper );
}

static inline qreal qskOffscreenBufferRatio( const QOpenGLContext* context )
{
    if ( context->screen() )
        return context->screen()->devicePixelRatio();

    return qGuiApp->devicePixelRatio();
}

qreal QskTextureRenderer::devicePixelRatio( const QOpenGLContext* context )
{
    if ( context == nullptr )
        context = QOpenGLContext::currentContext();

    qreal ratio = 1.0;

    if ( context->surface()->surfaceClass() == QSurface::Window )
    {
        auto* window = static_cast< QWindow* >( context->surface() );

        if ( auto* quickWindow = qobject_cast< QQuickWindow* >( window ) )
            ratio = quickWindow->effectiveDevicePixelRatio();
        else
            ratio = window->devicePixelRatio();
    }
    else
    {
        ratio = qskOffscreenBufferRatio( context );
    }

    return ratio;
}

