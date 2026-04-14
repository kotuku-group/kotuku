<xsl:stylesheet
  version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns="http://www.w3.org/1999/xhtml">

  <!-- python3 -m http.server -d /kotuku/docs/xml -->

  <xsl:output
    doctype-public="-//W3C//DTD XHTML 1.1//EN"
    doctype-system="http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"
    method="html" encoding="utf-8"
    omit-xml-declaration="yes" indent="no"/>

  <xsl:template match="constants">
    <xsl:choose>
      <xsl:when test="const">
        <table class="table">
          <thead><tr><th>Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="const">
              <tr><th class="col-md-1"><xsl:value-of select="../@prefix"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:when>
      <xsl:otherwise>
        <xsl:variable name="prefix"><xsl:value-of select="@prefix"/></xsl:variable>
        <table class="table">
          <thead><tr><th>Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="/book/types/constants[@lookup=$prefix]/const">
              <tr><th class="col-md-1"><xsl:value-of select="../@lookup"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="types">
    <xsl:choose>
      <xsl:when test="type">
        <table class="table">
          <thead><tr><th>Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="type">
              <xsl:choose>
                <xsl:when test="../@lookup">
                  <tr><th class="col-md-1"><xsl:value-of select="../@lookup"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
                </xsl:when>
                <xsl:otherwise>
                  <tr><th class="col-md-1"><xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
                </xsl:otherwise>
              </xsl:choose>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:when>
      <xsl:otherwise>
        <xsl:variable name="prefix"><xsl:value-of select="@lookup"/></xsl:variable>
        <table class="table">
          <thead><tr><th>Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="/book/types/constants[@lookup=$prefix]/const">
              <tr><th><xsl:value-of select="../@lookup"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="text()"><xsl:value-of select="."/></xsl:template>

  <xsl:template match="p|b|li">
    <xsl:element name="{name()}" xmlns="http://www.w3.org/1999/xhtml">
      <xsl:copy-of select="@*"/>
      <xsl:apply-templates select="*|text()"/>
    </xsl:element>
  </xsl:template>

  <xsl:template match="st"> <!-- Struct reference -->
    <xsl:variable name="structName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/structs/struct[name=$structName]/comment"/></xsl:attribute><xsl:attribute name="href">?page=struct-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
  </xsl:template>

  <xsl:template match="lk"> <!-- Type reference -->
    <xsl:variable name="typeName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/structs/struct[name=$typeName]/comment"/></xsl:attribute><xsl:attribute name="href">?page=<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
  </xsl:template>

  <xsl:template match="function">
    <xsl:choose>
      <xsl:when test="@module">
        <xsl:variable name="mod_name"><xsl:value-of select="@module"/></xsl:variable>
        <xsl:variable name="mod_lower" select="translate($mod_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>
        <a><xsl:attribute name="href"><xsl:value-of select="$mod_lower"/>.html?page=<xsl:value-of select="."/></xsl:attribute><xsl:value-of select="."/>()</a>
      </xsl:when>
      <xsl:otherwise>
        <a><xsl:attribute name="href">?page=<xsl:value-of select="."/></xsl:attribute><xsl:value-of select="."/>()</a>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="list">
    <xsl:choose>
      <xsl:when test="@type='ordered'">
        <ol><xsl:apply-templates select="*|node()"/></ol>
      </xsl:when>
      <xsl:otherwise>
        <ul><xsl:apply-templates select="*|node()"/></ul>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="class">
    <xsl:variable name="class_name"><xsl:value-of select="@name"/></xsl:variable>
    <xsl:variable name="class_lower" select="translate($class_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>

    <xsl:choose>
      <xsl:when test="@field">
        <a><xsl:attribute name="href">classes/<xsl:value-of select="$class_lower"/>.html#tf-<xsl:value-of select="@field"/></xsl:attribute><xsl:value-of select="@name"/>&#8658;<xsl:value-of select="@field"/></a>
      </xsl:when>
      <xsl:when test="@method">
        <a><xsl:attribute name="href">classes/<xsl:value-of select="$class_lower"/>.html#tm-<xsl:value-of select="@method"/></xsl:attribute><xsl:value-of select="@name"/>&#8658;<xsl:value-of select="@method"/>()</a>
      </xsl:when>
      <xsl:otherwise>
        <a><xsl:attribute name="href">classes/<xsl:value-of select="$class_lower"/>.html</xsl:attribute><xsl:value-of select="@name"/></a>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="include">
    <code><xsl:value-of select="."/></code>
  </xsl:template>

  <xsl:template match="header">
    <h4><xsl:value-of select="."/></h4>
  </xsl:template>

  <xsl:template match="code">
    <xsl:element name="{name()}" xmlns="http://www.w3.org/1999/xhtml">
      <xsl:apply-templates select="*|text()" />
    </xsl:element>
  </xsl:template>

  <xsl:template match="pre"> <!-- Note that pre areas can legitimately use elements like 'b' for visual enhancement -->
    <xsl:element name="{name()}" xmlns="http://www.w3.org/1999/xhtml">
      <xsl:apply-templates select="*|node()"/>
    </xsl:element>
  </xsl:template>

  <xsl:template name="addGoogleTracking">
    <!-- Global site tag (gtag.js) - Google Analytics -->
	 <script type="text/javascript" async="async" src="https://www.googletagmanager.com/gtag/js?id=G-8254DG7MT6"><xsl:text> </xsl:text></script>
	 <script type="text/javascript">
	   <xsl:text disable-output-escaping="yes">
	   window.dataLayer = window.dataLayer || [];
	   function gtag(){dataLayer.push(arguments);}
	   gtag('js', new Date());
      gtag('config', 'G-8254DG7MT6');</xsl:text>
	 </script>
  </xsl:template>

  <xsl:template match="/book">
    <html xmlns="http://www.w3.org/1999/xhtml" lang="en">
      <head>
        <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
        <xsl:call-template name="addGoogleTracking"/>
        <meta name="viewport" content="width=device-width, initial-scale=1"/>
        <!-- The above 2 meta tags *must* come first in the head; any other head content must come *after* these tags -->
        <meta name="description" content="Kōtuku documentation, machine generated from source"/>
        <meta name="author" content="Paul Manias"/>
        <link rel="icon" href="/favicon.ico"/>
        <title>Kōtuku Manual</title>
        <link href="../css/bootstrap.min.css" rel="stylesheet"/>
        <link href="../css/module-template.css" rel="stylesheet"/>
      </head>

      <body>
        <nav class="navbar navbar-expand-sm navbar-dark bg-dark">
          <div class="container-fluid">
            <div class="navbar-header"><a class="navbar-brand" href="../index.html">Kōtuku</a></div>
            <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbar" aria-controls="navbar" aria-expanded="false" aria-label="Toggle navigation">
              <span class="navbar-toggler-icon"></span>
            </button><xsl:text>&#xa;</xsl:text>
            <div id="navbar" class="collapse navbar-collapse">
              <ul class="nav navbar-nav">
                <li class="nav-item"><a class="nav-link" href="../gallery.html">Gallery</a></li>
                <li class="nav-item"><a class="nav-link" href="api.html">API</a></li>
                <li class="nav-item"><a class="nav-link" href="../wiki/Home.html">Wiki</a></li>
                <li class="nav-item"><a class="nav-link" href="https://github.com/parasol-framework/kotuku">GitHub</a></li>
              </ul><xsl:text>&#xa;</xsl:text>
            </div> <!-- nav-collapse -->
          </div>
        </nav>
        <xsl:text>&#xa;</xsl:text>
        <div class="container-fluid"> <!-- 'container-fluid' for full width, 'container' for restricted -->
          <div class="row">

            <!-- SIDEBAR -->
            <div class="d-sm-block d-none col-3 sidebar" style="max-width: 230px;">
              <div class="flex-shrink-1 pt-2 pe-2 sticky-top overflow-auto vh-100 b-shadow">

<ul class="list-unstyled">
  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#func-collapse" aria-expanded="true"><xsl:value-of select="/book/info/name"/> API</button>
    <div class="collapse show" id="func-collapse"><xsl:text>&#xa;</xsl:text>
      <ul class="btn-toggle-nav list-unstyled fw-normal"><xsl:text>&#xa;</xsl:text>
        <li><a class="rounded" role="button"><xsl:attribute name="onclick">showPage('default-page');</xsl:attribute><i class="bi bi-house"/><xsl:text>&#160;</xsl:text>Overview</a></li>
        <li class="border-top my-1"></li> <!-- Line break -->
        <xsl:text>&#xa;</xsl:text>
        <xsl:if test="count(/book/*/category) = 0">
          <xsl:for-each select="/book/function[not(category)]"><li><a role="button" class="rounded"><xsl:attribute name="onclick">showPage('<xsl:value-of select="name"/>');</xsl:attribute><xsl:value-of select="name"/>()</a></li></xsl:for-each>
        </xsl:if>
        <xsl:text>&#xa;</xsl:text>
        <xsl:for-each select="info/categories/category">
          <xsl:variable name="category"><xsl:value-of select="."/></xsl:variable>
          <xsl:variable name="id-category" select="translate($category,' ','_')"/>
          <li><ul class="list-unstyled ps-3">
             <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" aria-expanded="false"><xsl:attribute name="data-bs-target">#<xsl:value-of select="."/>-collapse</xsl:attribute><xsl:value-of select="."/></button>
               <div class="collapse">
                 <xsl:attribute name="id"><xsl:value-of select="."/>-collapse</xsl:attribute>
                 <ul class="btn-toggle-nav list-unstyled fw-normal pb-1 ps-3">
                   <xsl:for-each select="/book/function[category=$category]">
                     <li><a class="rounded" role="button"><xsl:attribute name="onclick">showPage('<xsl:value-of select="name"/>');</xsl:attribute><xsl:value-of select="name"/>()</a></li>
                     <xsl:text>&#xa;</xsl:text>
                   </xsl:for-each>
                 </ul><xsl:text>&#xa;</xsl:text>
               </div><xsl:text>&#xa;</xsl:text>
             </li><xsl:text>&#xa;</xsl:text>
           </ul></li><xsl:text>&#xa;</xsl:text>
           <xsl:text>&#xa;</xsl:text>
        </xsl:for-each> <!-- Category -->
      </ul>
    </div>
  </li>
</ul>

<!-- MODULES -->

<ul class="list-unstyled">
  <li class="border-top my-3"></li> <!-- Line break -->
  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#mod-collapse" aria-expanded="false">Modules</button>
    <div class="collapse" id="mod-collapse">
      <ul class="btn-toggle-nav list-unstyled fw-normal">
        <li class="api-ref"><a href="audio.html" class="rounded">Audio</a></li>
        <li class="api-ref"><a href="core.html" class="rounded">Core</a></li>
        <li class="api-ref"><a href="display.html" class="rounded">Display</a></li>
        <li class="api-ref"><a href="font.html" class="rounded">Font</a></li>
        <li class="api-ref"><a href="network.html" class="rounded">Network</a></li>
        <li class="api-ref"><a href="regex.html" class="rounded">Regex</a></li>
        <li class="api-ref"><a href="tiri.html" class="rounded">Tiri</a></li>
        <li class="api-ref"><a href="vector.html" class="rounded">Vector</a></li>
        <li class="api-ref"><a href="xml.html" class="rounded">XML</a></li>
        <li class="api-ref"><a href="xpath.html" class="rounded">XPath</a></li>
      </ul>
    </div>
  </li>
</ul>

<!-- CLASS LIST -->

<ul class="list-unstyled">
  <li class="border-top my-3"></li> <!-- Line break -->
  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#class-collapse" aria-expanded="false">Classes</button>
    <div class="collapse" id="class-collapse">
      <ul class="btn-toggle-nav list-unstyled fw-normal pb-1 ps-3">
        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#audio-collapse" aria-expanded="false">Audio</button>
          <div class="collapse" id="audio-collapse">
            <ul class="btn-toggle-nav list-unstyled fw-normal pb-1">
              <li><a href="classes/audio.html" class="rounded">Audio</a></li>
              <li><a href="classes/sound.html" class="rounded">Sound</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#core-collapse" aria-expanded="false">Core</button>
          <div class="collapse" id="core-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a href="classes/file.html" class="rounded">File</a></li>
              <li><a href="classes/metaclass.html" class="rounded">MetaClass</a></li>
              <li><a href="classes/module.html" class="rounded">Module</a></li>
              <li><a href="classes/storagedevice.html" class="rounded">StorageDevice</a></li>
              <li><a href="classes/task.html" class="rounded">Task</a></li>
              <li><a href="classes/thread.html" class="rounded">Thread</a></li>
              <li><a href="classes/time.html" class="rounded">Time</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#data-collapse" aria-expanded="false">Data</button>
          <div class="collapse" id="data-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a href="classes/compression.html" class="rounded">Compression</a></li>
              <li><a href="classes/compressedstream.html" class="rounded">CompressedStream</a></li>
              <li><a href="classes/config.html" class="rounded">Config</a></li>
              <li><a href="classes/script.html" class="rounded">Script</a></li>
              <li><a href="classes/xml.html" class="rounded">XML</a></li>
              <li><a href="classes/xquery.html" class="rounded">XQuery</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#devices-collapse" aria-expanded="false">Devices</button>
          <div class="collapse" id="devices-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a href="classes/controller.html" class="rounded">Controller</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#effects-collapse" aria-expanded="false">Effects</button>
          <div class="collapse" id="effects-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a class="rounded" href="classes/blurfx.html">BlurFX</a></li>
              <li><a class="rounded" href="classes/colourfx.html">ColourFX</a></li>
              <li><a class="rounded" href="classes/compositefx.html">CompositeFX</a></li>
              <li><a class="rounded" href="classes/convolvefx.html">ConvolveFX</a></li>
              <li><a class="rounded" href="classes/displacementfx.html">DisplacementFX</a></li>
              <li><a class="rounded" href="classes/filtereffect.html">FilterEffect</a></li>
              <li><a class="rounded" href="classes/floodfx.html">FloodFX</a></li>
              <li><a class="rounded" href="classes/imagefx.html">ImageFX</a></li>
              <li><a class="rounded" href="classes/lightingfx.html">LightingFX</a></li>
              <li><a class="rounded" href="classes/mergefx.html">MergeFX</a></li>
              <li><a class="rounded" href="classes/morphologyfx.html">MorphologyFX</a></li>
              <li><a class="rounded" href="classes/offsetfx.html">OffsetFX</a></li>
              <li><a class="rounded" href="classes/remapfx.html">RemapFX</a></li>
              <li><a class="rounded" href="classes/sourcefx.html">SourceFX</a></li>
              <li><a class="rounded" href="classes/turbulencefx.html">TurbulenceFX</a></li>
              <li><a class="rounded" href="classes/wavefunctionfx.html">WaveFunctionFX</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#ext-collapse" aria-expanded="false">Extensions</button>
          <div class="collapse" id="ext-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a class="rounded" href="classes/scintilla.html">Scintilla</a></li>
              <li><a class="rounded" href="classes/scintillasearch.html">ScintillaSearch</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#gfx-collapse" aria-expanded="false">Graphics</button>
          <div class="collapse" id="gfx-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a class="rounded" href="classes/bitmap.html">Bitmap</a></li>
              <li><a class="rounded" href="classes/clipboard.html">Clipboard</a></li>
              <li><a class="rounded" href="classes/display.html">Display</a></li>
              <li><a class="rounded" href="classes/document.html">Document</a></li>
              <li><a class="rounded" href="classes/font.html">Font</a></li>
              <li><a class="rounded" href="classes/picture.html">Picture</a></li>
              <li><a class="rounded" href="classes/pointer.html">Pointer</a></li>
              <li><a class="rounded" href="classes/surface.html">Surface</a></li>
              <li><a class="rounded" href="classes/svg.html">SVG</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#net-collapse" aria-expanded="false">Network</button>
          <div class="collapse" id="net-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a class="rounded" href="classes/clientsocket.html">ClientSocket</a></li>
              <li><a class="rounded" href="classes/http.html">HTTP</a></li>
              <li><a class="rounded" href="classes/netclient.html">NetClient</a></li>
              <li><a class="rounded" href="classes/netlookup.html">NetLookup</a></li>
              <li><a class="rounded" href="classes/netsocket.html">NetSocket</a></li>
              <li><a class="rounded" href="classes/proxy.html">Proxy</a></li>
            </ul>
          </div>
        </li>

        <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#vectors-collapse" aria-expanded="false">Vectors</button>
          <div class="collapse" id="vectors-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li><a class="rounded" href="classes/vector.html">Vector</a></li>
              <li><a class="rounded" href="classes/vectorclip.html">VectorClip</a></li>
              <li><a class="rounded" href="classes/vectorcolour.html">VectorColour</a></li>
              <li><a class="rounded" href="classes/vectorellipse.html">VectorEllipse</a></li>
              <li><a class="rounded" href="classes/vectorfilter.html">VectorFilter</a></li>
              <li><a class="rounded" href="classes/vectorgradient.html">VectorGradient</a></li>
              <li><a class="rounded" href="classes/vectorgroup.html">VectorGroup</a></li>
              <li><a class="rounded" href="classes/vectorimage.html">VectorImage</a></li>
              <li><a class="rounded" href="classes/vectorpath.html">VectorPath</a></li>
              <li><a class="rounded" href="classes/vectorpattern.html">VectorPattern</a></li>
              <li><a class="rounded" href="classes/vectorpolygon.html">VectorPolygon</a></li>
              <li><a class="rounded" href="classes/vectorrectangle.html">VectorRectangle</a></li>
              <li><a class="rounded" href="classes/vectorscene.html">VectorScene</a></li>
              <li><a class="rounded" href="classes/vectorshape.html">VectorShape</a></li>
              <li><a class="rounded" href="classes/vectorspiral.html">VectorSpiral</a></li>
              <li><a class="rounded" href="classes/vectortext.html">VectorText</a></li>
              <li><a class="rounded" href="classes/vectortransition.html">VectorTransition</a></li>
              <li><a class="rounded" href="classes/vectorviewport.html">VectorViewport</a></li>
              <li><a class="rounded" href="classes/vectorwave.html">VectorWave</a></li>
            </ul>
          </div>
        </li>
      </ul>
    </div>
  </li>
</ul> <!-- Classes -->

<!-- WIKI -->

<ul class="list-unstyled">
  <li class="border-top my-3"></li> <!-- Line break -->
  <li>
    <button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#wiki-collapse" aria-expanded="false">Wiki</button>
    <div class="collapse" id="wiki-collapse">
      <ul class="btn-toggle-nav list-unstyled fw-normal pb-1 ps-3">
         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#bp-collapse" aria-expanded="false">Build Process</button>
         <div class="collapse" id="bp-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li class="api-ref"><a class="rounded" href="../wiki/Linux-Builds.html">Linux Builds</a></li>
              <li class="api-ref"><a class="rounded" href="../wiki/Windows-Builds.html">Windows Builds</a></li>
              <li class="api-ref"><a class="rounded" href="../wiki/Customising-Your-Build.html">Customising Your Build</a></li>
            </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#tm-collapse" aria-expanded="false">Technical Manuals</button>
         <div class="collapse" id="tm-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
             <li class="api-ref"><a class="rounded" href="../wiki/Kotuku-Objects.html">Kōtuku Objects</a></li>
             <li class="api-ref"><a class="rounded" href="../wiki/Kotuku-In-Depth.html">Kōtuku In Depth</a></li>
           </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#fg-collapse" aria-expanded="false">Tiri</button>
         <div class="collapse" id="fg-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
            <li class="api-ref"><a class="rounded" href="../wiki/Tiri-Reference-Manual.html">Tiri Reference Manual</a></li>
            <li class="api-ref"><a class="rounded" href="../wiki/Tiri-FileSearch-API.html">FileSearch API</a></li>
            <li class="api-ref"><a class="rounded" href="../wiki/Tiri-GUI-API.html">GUI API</a></li>
            <li class="api-ref"><a class="rounded" href="../wiki/Tiri-JSON-API.html">JSON API</a></li>
            <li class="api-ref"><a class="rounded" href="../wiki/Tiri-VFX-API.html">VFX API</a></li>
            <li class="api-ref"><a class="rounded" href="../wiki/Widgets.html">Widgets</a></li>
           </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#rrm-collapse" aria-expanded="false">RIPL</button>
         <div class="collapse" id="rrm-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li class="api-ref"><a class="rounded" href="../wiki/RIPL-Reference-Manual.html">RIPL Reference Manual</a></li>
           </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#tools-collapse" aria-expanded="false">Tools</button>
         <div class="collapse" id="tools-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li class="api-ref"><a class="rounded" href="../wiki/Origo.html">Origo</a></li>
              <li class="api-ref"><a class="rounded" href="../wiki/Unit-Testing.html">Flute / Unit Testing</a></li>
           </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#doc-collapse" aria-expanded="false">Doc Generation</button>
         <div class="collapse" id="doc-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
              <li class="api-ref"><a class="rounded" href="../wiki/Embedded-Document-Formatting.html">Embedded Document Format</a></li>
              <li class="api-ref"><a class="rounded" href="../wiki/TDL-Reference-Manual.html">TDL Reference Manual</a></li>
              <li class="api-ref"><a class="rounded" href="../wiki/TDL-Tools.html">TDL Tools</a></li>
           </ul>
         </div></li>

         <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#app-collapse" aria-expanded="false">Appendix</button>
         <div class="collapse" id="app-collapse">
            <ul class="btn-toggle-nav list-unstyled pb-1">
             <li class="api-ref"><a class="rounded" href="../wiki/Action-Reference-Manual.html">Action Reference Manual</a></li>
             <li class="api-ref"><a class="rounded" href="../wiki/System-Error-Codes.html">System Error Codes</a></li>
           </ul>
         </div></li>
      </ul>
    </div>
  </li>
</ul>
              </div>
            </div>

            <!-- DEFAULT BODY -->
            <div class="col-sm-9" style="max-width: 1200px;">
              <div class="docs-content" style="display:none;" id="default-page">
                <div class="page-header"><h1><xsl:value-of select="/book/info/name"/> Module</h1></div>

                <p class="lead"><xsl:value-of select="/book/info/comment"/></p>
                <xsl:for-each select="/book/info/description">
                  <xsl:apply-templates/>
                  <xsl:text>&#xa;</xsl:text>
                </xsl:for-each>

                <h3>Functions</h3>

                <!-- Non-categorised functions -->
                <xsl:if test="count(/book/*/category) = 0">
                  <p class="appendix"><xsl:for-each select="/book/function">
                    <a role="button"><xsl:attribute name="onclick">showPage('<xsl:value-of select="name"/>');</xsl:attribute><xsl:value-of select="name"/></a><xsl:if test="position() != last()"><xsl:text>&#160;</xsl:text>| </xsl:if>
                  </xsl:for-each></p>
                </xsl:if>

                <!-- Categorised functions -->
                <xsl:for-each select="info/categories/category">
                  <xsl:variable name="category"><xsl:value-of select="."/></xsl:variable>
                  <xsl:variable name="id-category" select="translate($category,' ','_')"/>
                  <h6><xsl:value-of select="."/></h6>
                  <p class="appendix ps-4"><xsl:for-each select="/book/function[category=$category]">
                    <a role="button"><xsl:attribute name="onclick">showPage('<xsl:value-of select="name"/>');</xsl:attribute><xsl:value-of select="name"/></a><xsl:if test="position() != last()"><xsl:text>&#160;</xsl:text>| </xsl:if>
                  </xsl:for-each></p>
                </xsl:for-each>

                <xsl:if test="count(structs/struct) > 0">
                  <h3>Structures</h3>
                  <p class="appendix"><xsl:for-each select="structs/struct">
                    <xsl:sort select="@name"/>
                    <a role="button"><xsl:attribute name="onclick">showPage('struct-<xsl:value-of select="@name"/>');</xsl:attribute><xsl:value-of select="@name"/></a><xsl:if test="position() != last()"><xsl:text>&#160;</xsl:text>| </xsl:if>
                  </xsl:for-each></p>
                </xsl:if>

                <xsl:if test="count(info/classes/class) > 0">
                  <h3>Classes</h3>
                  <p class="appendix"><xsl:for-each select="info/classes/class">
                    <xsl:variable name="class_name"><xsl:value-of select="."/></xsl:variable>
                    <xsl:variable name="lower" select="translate($class_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>
                    <a role="button" onclick=""><xsl:attribute name="href">classes/<xsl:value-of select="$lower"/>.html</xsl:attribute><xsl:value-of select="."/></a><xsl:if test="position() != last()"><xsl:text>&#160;</xsl:text>| </xsl:if>
                  </xsl:for-each></p>
                </xsl:if>

                <xsl:if test="count(types/constants) > 0">
                  <h3>Constants</h3>
                  <p class="appendix"><xsl:for-each select="types/constants">
                    <xsl:sort select="@lookup"/>
                    <a role="button"><xsl:attribute name="onclick">showPage('<xsl:value-of select="@lookup"/>');</xsl:attribute><xsl:value-of select="@lookup"/></a><xsl:if test="position() != last()"><xsl:text>&#160;</xsl:text>| </xsl:if>
                  </xsl:for-each></p>
                </xsl:if>
              </div>

              <!-- FUNCTION BODY -->
              <xsl:for-each select="function">
                <div class="docs-content" style="display:none;">
                  <xsl:attribute name="id"><xsl:value-of select="name"/></xsl:attribute>

                  <h2><xsl:value-of select="name"/>()</h2>
                  <p class="lead"><xsl:value-of select="comment"/></p>
                  <div class="card card-info mb-3">
                    <div class="card-header"><samp><xsl:value-of select="prototype"/></samp></div>

                    <xsl:choose>
                      <xsl:when test="input/param">
                        <div class="card-body" style="padding:0px">
                          <table class="table mb-3 thead-light">
                            <thead>
                              <tr><th class="col-md-1">Parameter</th><th>Description</th></tr>
                            </thead>
                            <tbody>
                              <xsl:for-each select="input/param">
                                <xsl:choose>
                                  <xsl:when test="@lookup">
                                    <tr><td><a><xsl:attribute name="onclick">showPage('<xsl:value-of select="@lookup"/>');</xsl:attribute><xsl:value-of select="@name"/></a></td><td><xsl:apply-templates select="."/></td></tr>
                                  </xsl:when>
                                  <xsl:otherwise>
                                    <tr><td><xsl:value-of select="@name"/></td><td><xsl:apply-templates select="."/></td></tr>
                                  </xsl:otherwise>
                                </xsl:choose>
                                <xsl:text>&#xa;</xsl:text>
                              </xsl:for-each>
                            </tbody>
                          </table>
                        </div>
                      </xsl:when>
                    </xsl:choose>
                  </div>

                  <xsl:for-each select="description">
                    <xsl:apply-templates/>
                  </xsl:for-each>

                  <xsl:choose>
                    <xsl:when test="result/error">
                      <h3>Error Codes</h3>
                      <table class="table table-sm borderless">
                        <tbody>
                          <xsl:for-each select="result/error">
                            <tr><th class="col-md-1"><xsl:value-of select="@code"/></th><td><xsl:apply-templates select="."/></td></tr>
                            <xsl:text>&#xa;</xsl:text>
                          </xsl:for-each>
                        </tbody>
                      </table>
                    </xsl:when>
                    <xsl:when test="result">
                      <h3>Result</h3>
                      <p><xsl:apply-templates select="result/."/></p>
                    </xsl:when>
                  </xsl:choose>

                  <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> module documentation © <xsl:value-of select="/book/info/copyright"/></div>
                </div>
                <xsl:text>&#xa;</xsl:text>
              </xsl:for-each> <!-- End of function scan -->

              <!-- TYPES -->

              <xsl:if test="count(types/constants) > 0">
                <xsl:for-each select="types/constants">
                  <div class="docs-content" style="display:none;">
                    <xsl:attribute name="id"><xsl:value-of select="@lookup"/></xsl:attribute>
                    <h1><xsl:value-of select="@lookup"/> Type</h1>
                    <p class="lead"><xsl:apply-templates select="@comment"/></p>
                    <xsl:if test="count(const) > 0">
                      <table class="table">
                        <thead><tr><th class="col-md-1">Name</th><th>Description</th></tr></thead>
                        <tbody>
                          <xsl:for-each select="const">
                            <tr><th><xsl:value-of select="../@lookup"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
                            <xsl:text>&#xa;</xsl:text>
                          </xsl:for-each>
                        </tbody>
                      </table>
                    </xsl:if>
                    <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> module documentation © <xsl:value-of select="/book/info/copyright"/></div>
                  </div>
                  <xsl:text>&#xa;</xsl:text>
                </xsl:for-each> <!-- End of type scan -->
              </xsl:if>

              <!-- STRUCTURES -->
              <xsl:for-each select="structs/struct">
                <div class="docs-content" style="display:none;">
                  <xsl:attribute name="id">struct-<xsl:value-of select="@name"/></xsl:attribute>
                  <h1><xsl:value-of select="@name"/> Structure</h1>
                  <p class="lead"><xsl:apply-templates select="@comment"/></p>
                  <table class="table">
                    <thead><tr><th class="col-md-1">Field</th><th class="col-md-1">Type</th><th>Description</th></tr></thead>
                    <tbody>
                      <xsl:for-each select="field">
                        <tr>
                          <th><xsl:value-of select="@name"/></th>
                          <td><span class="text-nowrap"><xsl:value-of select="@type"/></span></td>
                          <td><xsl:apply-templates select="."/></td>
                        </tr>
                        <xsl:text>&#xa;</xsl:text>
                      </xsl:for-each>
                    </tbody>
                  </table>
                  <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> module documentation © <xsl:value-of select="/book/info/copyright"/></div>
                </div>
                <xsl:text>&#xa;</xsl:text>
              </xsl:for-each> <!-- End of struct scan -->
            </div> <!-- End of core content -->
          </div> <!-- row -->
        </div> <!-- container -->

        <script type="text/javascript" src="../js/bootstrap.bundle.min.js"></script>
        <script type="text/javascript" src="../js/base.js"></script>
        <script type="text/javascript"><xsl:text disable-output-escaping="yes">
var glCurrentMethod;

const ready = fn => document.readyState !== 'loading' ? fn() : document.addEventListener('DOMContentLoaded', fn);

   var xslt = false;
   var url = window.location.pathname;
   var filename = url.substring(url.lastIndexOf('/')+1);
   if (filename.endsWith(".xml")) { // XSLT is being used to view this document
      filename = filename.substr(0, filename.length-3) + 'html';
      xslt = true;
   }

ready(function(){
   // Initialise tooltips
   var tooltipTriggerList = [].slice.call(document.querySelectorAll('[data-bs-toggle="tooltip"]'))
   var tooltipList = tooltipTriggerList.map(function (tooltipTriggerEl) {
     return new bootstrap.Tooltip(tooltipTriggerEl)
   })

   glCurrentMethod = document.getElementById("Introduction");

   var page = glParameters["page"];
   if (isEmpty(page)) page = glParameters["function"];

   if (isEmpty(page)) showPage("default-page", true);
   else showPage(page, true);

   window.onpopstate = popState;

   // In XSLT mode, changing all HTML links to XML is helpful for navigation (if heavy handed)
   if (xslt) {
      var nl = document.querySelectorAll('a[href$=".html"]');
      nl.forEach((el) => {
         el.href = el.href.substr(0, el.href.length-5) + '.xml'
      })
   }
});

function popState(event) {
   console.log("popState() to " + JSON.stringify(event.state));

   state = event.state
   if (!state) state = { page: 'default-page' }

   var div
   if (state.page) div = document.getElementById(state.page);
   else div =  document.getElementById('default-page');

   if (div) {
      if (glCurrentMethod) glCurrentMethod.style.display = "none"; // Hide previous method.
      div.style.display = "block"; // Show selected method.
      glCurrentMethod = div;
   }
   else console.log("Div for '" + state.page + "' not found.");
}

function showPage(Name, NoHistory)
{
   var div = document.getElementById(Name);
   if (div) {
      if (glCurrentMethod) glCurrentMethod.style.display = "none"; // Hide previous method.
      div.style.display = "block"; // Show selected method.
      glCurrentMethod = div;

      if (!NoHistory) {
         history.pushState({ page: Name }, null, "?page=" + Name);
      }
      window.scrollTo(0, 0)
   }
   else console.log("Div for '" + Name + "' not found.");

   return false;
}
         </xsl:text>
        </script>
      </body>
    </html>
  </xsl:template>
</xsl:stylesheet>
