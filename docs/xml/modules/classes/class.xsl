<xsl:stylesheet
  version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns="http://www.w3.org/1999/xhtml">

  <xsl:output
    doctype-public="-//W3C//DTD XHTML 1.1//EN"
    doctype-system="http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"
    method="html" encoding="utf-8"
    omit-xml-declaration="yes" indent="no"/>

  <xsl:template match="types">
    <xsl:choose>
      <xsl:when test="type"> <!-- Build a one-off table of constant values -->
        <table class="table">
          <thead><tr><th>Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="type">
              <xsl:choose>
                <xsl:when test="../@prefix">
                  <tr><th class="col-md-1"><xsl:value-of select="../@prefix"/>::<xsl:value-of select="@name"/></th><td><xsl:value-of select="."/></td></tr>
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
      <xsl:when test="@lookup"> <!-- Build a table of constant values from the dictionary -->
        <xsl:variable name="prefix"><xsl:value-of select="@lookup"/></xsl:variable>
        <table class="table">
          <thead><tr><th class="col-md-1">Name</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="/book/types/constants[@lookup=$prefix]/const">
              <tr><th class="col-md-1"><xsl:value-of select="$prefix"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:when>
      <xsl:otherwise>
        <p class="text-danger">Incorrect types tag detected.</p>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="struct">
    <xsl:choose>
      <xsl:when test="@lookup"> <!-- Build a table of field values from the dictionary -->
        <xsl:variable name="prefix"><xsl:value-of select="@lookup"/></xsl:variable>
        <table class="table">
          <thead><tr><th class="col-md-1">Field</th><th>Type</th><th>Description</th></tr></thead>
          <tbody>
            <xsl:for-each select="/book/structs/struct[@name=$prefix]/field">
              <tr><th class="col-md-1"><xsl:value-of select="@name"/></th><td><xsl:value-of select="@type"/></td><td><xsl:apply-templates select="."/></td></tr>
              <xsl:text>&#xa;</xsl:text>
            </xsl:for-each>
          </tbody>
        </table>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="."/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="p|b|li">
    <xsl:element name="{name()}" xmlns="http://www.w3.org/1999/xhtml">
      <xsl:copy-of select="@*"/>
      <xsl:apply-templates select="*|text()"/>
    </xsl:element>
  </xsl:template>

  <xsl:template match="text()"><xsl:value-of select="."/></xsl:template>

  <xsl:template match="fl">
    <xsl:variable name="fieldName"><xsl:value-of select="node()"/></xsl:variable>
    <xsl:choose>
      <xsl:when test="@module">
        <xsl:variable name="mod_name"><xsl:value-of select="@module"/></xsl:variable>
        <xsl:variable name="mod_lower" select="translate($mod_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>
        <a><xsl:attribute name="href"><xsl:value-of select="$mod_lower"/>.html#tf-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
      </xsl:when>
      <xsl:otherwise>
        <a data-bs-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/fields/field[name=$fieldName]/comment"/></xsl:attribute><xsl:attribute name="href">#tf-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="st"> <!-- Struct reference -->
    <xsl:variable name="structName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-bs-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/structs/struct[name=$structName]/comment"/></xsl:attribute><xsl:attribute name="href">?page=struct-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
  </xsl:template>

  <xsl:template match="lk"> <!-- Type reference -->
    <xsl:variable name="typeName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-bs-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/structs/struct[name=$typeName]/comment"/></xsl:attribute><xsl:attribute name="href">?page=<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/></a>
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

  <xsl:template match="action"><xsl:variable name="actionName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-bs-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/actions/action[name=$actionName]/comment"/></xsl:attribute><xsl:attribute name="href">#ta-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/>()</a></xsl:template>

  <xsl:template match="method"><xsl:variable name="methodName"><xsl:value-of select="node()"/></xsl:variable>
    <a data-bs-toggle="tooltip"><xsl:attribute name="title"><xsl:value-of select="/book/methods/method[name=$methodName]/comment"/></xsl:attribute><xsl:attribute name="href">#tm-<xsl:value-of select="node()"/></xsl:attribute><xsl:value-of select="node()"/>()</a></xsl:template>

  <xsl:template match="class">
    <xsl:variable name="class_name"><xsl:value-of select="@name"/></xsl:variable>
    <xsl:variable name="class_lower" select="translate($class_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>

    <xsl:choose>
      <xsl:when test="@field">
        <a><xsl:attribute name="href"><xsl:value-of select="$class_lower"/>.html#tf-<xsl:value-of select="@field"/></xsl:attribute><xsl:value-of select="@name"/>&#8658;<xsl:value-of select="@field"/></a>
      </xsl:when>
      <xsl:when test="@method">
        <a><xsl:attribute name="href"><xsl:value-of select="$class_lower"/>.html#tm-<xsl:value-of select="@method"/></xsl:attribute><xsl:value-of select="@name"/>&#8658;<xsl:value-of select="@method"/>()</a>
      </xsl:when>
      <xsl:otherwise>
        <a><xsl:attribute name="href"><xsl:value-of select="$class_lower"/>.html</xsl:attribute><xsl:value-of select="@name"/></a>
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
    <html version="-//W3C//DTD XHTML 1.1//EN"
      xmlns="http://www.w3.org/1999/xhtml"
      xml:lang="en"
      xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
      xsi:schemaLocation="http://www.w3.org/MarkUp/SCHEMA/xhtml11.xsd">
      <head>
        <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
        <xsl:call-template name="addGoogleTracking"/>
        <meta name="viewport" content="width=device-width, initial-scale=1"/>
        <!-- The above 2 meta tags *must* come first in the head; any other head content must come *after* these tags -->
        <meta name="description" content="Kōtuku documentation, machine generated from source"/>
        <meta name="author" content="Paul Manias"/>
        <link rel="icon" href="/favicon.ico"/>
        <title>Kōtuku Manual</title>
        <link href="../../css/bootstrap.min.css" rel="stylesheet"/>
        <link href="../../css/module-template.css" rel="stylesheet"/>
        <script type="text/javascript">
          var shiftWindow = function() { scrollBy(0, -100) };
          window.addEventListener("hashchange", shiftWindow);
          function load() { if (window.location.hash) shiftWindow(); }
        </script>
      </head>

      <body>
        <nav class="navbar navbar-expand-sm navbar-dark bg-dark">
          <div class="container-fluid">
            <div class="navbar-header"><a class="navbar-brand" href="../../index.html">Kōtuku</a></div>
            <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbar" aria-controls="navbar" aria-expanded="false" aria-label="Toggle navigation">
              <span class="navbar-toggler-icon"></span>
            </button>
            <div id="navbar" class="collapse navbar-collapse">
              <ul class="nav navbar-nav">
                <li class="nav-item"><a class="nav-link" href="../../gallery.html">Gallery</a></li>
                <li class="nav-item"><a class="nav-link" href="../api.html">API</a></li>
                <li class="nav-item"><a class="nav-link" href="../../wiki/Home.html">Wiki</a></li>
                <li class="nav-item"><a class="nav-link" href="https://github.com/parasol-framework/kotuku">GitHub</a></li>
              </ul>
            </div> <!-- nav-collapse -->
          </div>
        </nav>
        <xsl:text>&#xa;</xsl:text>

        <div class="container-fluid"> <!-- 'container-fluid' for full width, 'container' for restricted -->
          <div class="row">

            <!-- SIDEBAR -->
            <div class="d-sm-block d-none col-3 sidebar" style="max-width: 230px;">
              <div class="flex-shrink-1 pt-2 pe-2 sticky-top overflow-auto vh-100 b-shadow">
                <xsl:text>&#xa;</xsl:text>
                <ul class="list-unstyled">
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#mod-collapse" aria-expanded="false">Modules</button>
                    <div class="collapse" id="mod-collapse">
                      <ul class="btn-toggle-nav list-unstyled fw-normal">
                        <li class="api-ref"><a class="rounded" href="../audio.html">Audio</a></li>
                        <li class="api-ref"><a class="rounded" href="../core.html">Core</a></li>
                        <li class="api-ref"><a class="rounded" href="../display.html">Display</a></li>
                        <li class="api-ref"><a class="rounded" href="../font.html">Font</a></li>
                        <li class="api-ref"><a class="rounded" href="../network.html">Network</a></li>
                        <li class="api-ref"><a class="rounded" href="../regex.html">Regex</a></li>
                        <li class="api-ref"><a class="rounded" href="../tiri.html">Tiri</a></li>
                        <li class="api-ref"><a class="rounded" href="../vector.html">Vector</a></li>
                        <li class="api-ref"><a class="rounded" href="../xml.html">XML</a></li>
                        <li class="api-ref"><a class="rounded" href="../xquery.html">XQuery</a></li>
                      </ul>
                    </div>
                  </li>
                </ul>
                <xsl:text>&#xa;</xsl:text>
                <ul class="list-unstyled">
                  <li class="border-top my-3"></li> <!-- Line break -->
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#audio-collapse" aria-expanded="false">Audio</button>
                    <div class="collapse" id="audio-collapse">
                      <ul class="btn-toggle-nav list-unstyled fw-normal pb-1">
                        <li class="api-ref"><a class="rounded" href="audio.html">Audio</a></li>
                        <li class="api-ref"><a class="rounded" href="sound.html">Sound</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#core-collapse" aria-expanded="false">Core</button>
                    <div class="collapse" id="core-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="file.html">File</a></li>
                        <li class="api-ref"><a class="rounded" href="metaclass.html">MetaClass</a></li>
                        <li class="api-ref"><a class="rounded" href="module.html">Module</a></li>
                        <li class="api-ref"><a class="rounded" href="storagedevice.html">StorageDevice</a></li>
                        <li class="api-ref"><a class="rounded" href="task.html">Task</a></li>
                        <li class="api-ref"><a class="rounded" href="thread.html">Thread</a></li>
                        <li class="api-ref"><a class="rounded" href="time.html">Time</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#data-collapse" aria-expanded="false">Data</button>
                    <div class="collapse" id="data-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="compression.html">Compression</a></li>
                        <li class="api-ref"><a class="rounded" href="compressedstream.html">CompressedStream</a></li>
                        <li class="api-ref"><a class="rounded" href="config.html">Config</a></li>
                        <li class="api-ref"><a class="rounded" href="script.html">Script</a></li>
                        <li class="api-ref"><a class="rounded" href="xml.html">XML</a></li>
                        <li class="api-ref"><a class="rounded" href="xquery.html">XQuery</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#devices-collapse" aria-expanded="false">Devices</button>
                    <div class="collapse" id="devices-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="controller.html">Controller</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#effects-collapse" aria-expanded="false">Effects</button>
                    <div class="collapse" id="effects-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="blurfx.html">BlurFX</a></li>
                        <li class="api-ref"><a class="rounded" href="colourfx.html">ColourFX</a></li>
                        <li class="api-ref"><a class="rounded" href="compositefx.html">CompositeFX</a></li>
                        <li class="api-ref"><a class="rounded" href="convolvefx.html">ConvolveFX</a></li>
                        <li class="api-ref"><a class="rounded" href="displacementfx.html">DisplacementFX</a></li>
                        <li class="api-ref"><a class="rounded" href="filtereffect.html">FilterEffect</a></li>
                        <li class="api-ref"><a class="rounded" href="floodfx.html">FloodFX</a></li>
                        <li class="api-ref"><a class="rounded" href="imagefx.html">ImageFX</a></li>
                        <li class="api-ref"><a class="rounded" href="lightingfx.html">LightingFX</a></li>
                        <li class="api-ref"><a class="rounded" href="mergefx.html">MergeFX</a></li>
                        <li class="api-ref"><a class="rounded" href="morphologyfx.html">MorphologyFX</a></li>
                        <li class="api-ref"><a class="rounded" href="offsetfx.html">OffsetFX</a></li>
                        <li class="api-ref"><a class="rounded" href="remapfx.html">RemapFX</a></li>
                        <li class="api-ref"><a class="rounded" href="sourcefx.html">SourceFX</a></li>
                        <li class="api-ref"><a class="rounded" href="turbulencefx.html">TurbulenceFX</a></li>
                        <li class="api-ref"><a class="rounded" href="wavefunctionfx.html">WaveFunctionFX</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#ext-collapse" aria-expanded="false">Extensions</button>
                    <div class="collapse" id="ext-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="scintilla.html">Scintilla</a></li>
                        <li class="api-ref"><a class="rounded" href="scintillasearch.html">ScintillaSearch</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#gfx-collapse" aria-expanded="false">Graphics</button>
                    <div class="collapse" id="gfx-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="bitmap.html">Bitmap</a></li>
                        <li class="api-ref"><a class="rounded" href="clipboard.html">Clipboard</a></li>
                        <li class="api-ref"><a class="rounded" href="display.html">Display</a></li>
                        <li class="api-ref"><a class="rounded" href="document.html">Document</a></li>
                        <li class="api-ref"><a class="rounded" href="font.html">Font</a></li>
                        <li class="api-ref"><a class="rounded" href="picture.html">Picture</a></li>
                        <li class="api-ref"><a class="rounded" href="pointer.html">Pointer</a></li>
                        <li class="api-ref"><a class="rounded" href="surface.html">Surface</a></li>
                        <li class="api-ref"><a class="rounded" href="svg.html">SVG</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#net-collapse" aria-expanded="false">Network</button>
                    <div class="collapse" id="net-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="clientsocket.html">ClientSocket</a></li>
                        <li class="api-ref"><a class="rounded" href="http.html">HTTP</a></li>
                        <li class="api-ref"><a class="rounded" href="netclient.html">NetClient</a></li>
                        <li class="api-ref"><a class="rounded" href="netlookup.html">NetLookup</a></li>
                        <li class="api-ref"><a class="rounded" href="netsocket.html">NetSocket</a></li>
                        <li class="api-ref"><a class="rounded" href="proxy.html">Proxy</a></li>
                      </ul>
                    </div>
                  </li>
                  <xsl:text>&#xa;</xsl:text>
                  <li><button class="btn btn-toggle align-items-center rounded collapsed" data-bs-toggle="collapse" data-bs-target="#vectors-collapse" aria-expanded="false">Vectors</button>
                    <div class="collapse" id="vectors-collapse">
                      <ul class="btn-toggle-nav list-unstyled pb-1">
                        <li class="api-ref"><a class="rounded" href="vector.html">Vector</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorclip.html">VectorClip</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorcolour.html">VectorColour</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorellipse.html">VectorEllipse</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorfilter.html">VectorFilter</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorgradient.html">VectorGradient</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorgroup.html">VectorGroup</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorimage.html">VectorImage</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorpath.html">VectorPath</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorpattern.html">VectorPattern</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorpolygon.html">VectorPolygon</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorrectangle.html">VectorRectangle</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorscene.html">VectorScene</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorshape.html">VectorShape</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorspiral.html">VectorSpiral</a></li>
                        <li class="api-ref"><a class="rounded" href="vectortext.html">VectorText</a></li>
                        <li class="api-ref"><a class="rounded" href="vectortransition.html">VectorTransition</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorviewport.html">VectorViewport</a></li>
                        <li class="api-ref"><a class="rounded" href="vectorwave.html">VectorWave</a></li>
                      </ul>
                    </div>
                  </li>
                </ul>
              </div>
            </div>
            <xsl:text>&#xa;</xsl:text>
            <div class="col-sm-9" style="max-width: 1200px;">
              <div class="docs-content" style="display:none;" id="default-page">
                <div class="page-header"><h1><xsl:value-of select="/book/info/name"/> Class</h1></div>
                <p class="lead"><xsl:value-of select="/book/info/comment"/></p>
                <xsl:for-each select="/book/info/description">
                  <xsl:apply-templates/>
                  <xsl:text>&#xa;</xsl:text>
                </xsl:for-each>

                <xsl:if test="/book/fields/field">
                <h3>Structure</h3>
                <p>The <xsl:value-of select="info/name"/> class consists of the following fields:</p>
                <table class="table">
                  <thead><th class="col-md-1"><div data-bs-toggle="tooltip"  title="Read/Write access indicators are listed below">Access</div></th><th class="col-md-1">Name</th><th class="col-md-1">Type</th><th>Comment</th></thead>
                  <tbody>
                    <xsl:for-each select="/book/fields/field">
                      <tr data-bs-toggle="collapse" data-bs-target="_" style="scroll-margin:3em;">
                        <xsl:attribute name="data-bs-target">#fl-<xsl:value-of select="name"/></xsl:attribute>
                        <xsl:if test="description or input">
                          <xsl:attribute name="role">button</xsl:attribute>
                        </xsl:if>
                        <td class="col-md-1">
                          <xsl:attribute name="id">tf-<xsl:value-of select="name"/></xsl:attribute>
                          <xsl:choose>
                            <xsl:when test="access/@read='R'"><i class="bi bi-check-circle-fill text-success" data-bs-toggle="tooltip" title="Direct read access"/></xsl:when>
                            <xsl:when test="access/@read='G'"><i class="bi bi-check-circle text-success" data-bs-toggle="tooltip" title="Functional read access"/></xsl:when>
                            <xsl:otherwise><i class="bi bi-dash-circle text-secondary" data-bs-toggle="tooltip" title="Not readable"/></xsl:otherwise>
                          </xsl:choose>
                          &#160;
                          <xsl:choose>
                            <xsl:when test="access/@write='W'"><i class="bi bi-check-circle-fill text-success" data-bs-toggle="tooltip" title="Direct write access"/></xsl:when>
                            <xsl:when test="access/@write='S'"><i class="bi bi-check-circle text-success" data-bs-toggle="tooltip" title="Functional write access"/></xsl:when>
                            <xsl:when test="access/@write='I'"><i class="bi bi-check-lg text-success" data-bs-toggle="tooltip" title="Immutable"/></xsl:when>
                            <xsl:otherwise><i class="bi bi-dash-circle-fill text-secondary" data-bs-toggle="tooltip" title="Not writeable"/></xsl:otherwise>
                          </xsl:choose></td>
                        <th class="col-md-1"><xsl:value-of select="name"/></th>
                        <td class="col-md-1"><span class="text-nowrap">
                          <xsl:choose>
                            <xsl:when test="type/@class">
                              <xsl:variable name="class_name"><xsl:value-of select="type/@class"/></xsl:variable>
                              <xsl:variable name="lower" select="translate($class_name,'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')"/>
                              <a><xsl:attribute name="href"><xsl:value-of select="$lower"/>.html</xsl:attribute><xsl:value-of select="type"/></a>
                            </xsl:when>
                            <xsl:otherwise>
                              <xsl:value-of select="type"/>
                            </xsl:otherwise>
                          </xsl:choose>
                        </span></td>
                        <td><xsl:apply-templates select="comment"/></td>
                      </tr>
                      <xsl:if test="description">
                        <tr>
                          <td colspan="4" class="hiddenRow">
                            <div id="_" class="accordion-body collapse">
                              <xsl:attribute name="id">fl-<xsl:value-of select="name"/></xsl:attribute>
                              <div class="doc-content" style="margin:20px">
                                <xsl:for-each select="description">
                                  <xsl:apply-templates/><xsl:text>&#xa;</xsl:text>
                                </xsl:for-each>
                              </div>
                            </div>
                          </td>
                        </tr>
                      </xsl:if>
                      <xsl:text>&#xa;</xsl:text>
                    </xsl:for-each>
                  </tbody>
                </table>
                </xsl:if>

                <xsl:if test="/book/actions/action">
                  <h3>Actions</h3>
                  <p>The following actions are currently supported:</p>
                  <table class="table">
                    <tbody>
                      <xsl:for-each select="/book/actions/action">
                        <tr data-bs-toggle="collapse" data-bs-target="_" style="scroll-margin:3em;">
                          <xsl:attribute name="id">ta-<xsl:value-of select="name"/></xsl:attribute>
                          <xsl:attribute name="data-bs-target">#tac-<xsl:value-of select="name"/></xsl:attribute>
                          <xsl:if test="description or input">
                            <xsl:attribute name="role">button</xsl:attribute>
                          </xsl:if>
                          <td style="width:1%; border-top-style: none; border-bottom-style: none;">
                            <xsl:if test="description or input">
                              <i class="bi bi-chevron-double-right text-secondary"/>
                            </xsl:if>
                          </td>
                          <th class="col-md-1 text-primary"><xsl:value-of select="name"/></th>
                          <td><xsl:apply-templates select="comment"/></td>
                        </tr>
                        <xsl:if test="description or input">
                          <tr>
                            <td class="hiddenRow" style="width:1%; border-top-style: none; border-bottom-style: none;"/>
                            <td colspan="2" class="hiddenRow">
                              <div id="_" class="collapse">
                                <xsl:attribute name="id">tac-<xsl:value-of select="name"/></xsl:attribute>
                                <div class="card" style="border-radius: 0;">
                                  <div class="card-header action-header" style="border-radius: 0;"><samp><xsl:value-of select="prototype"/></samp></div>
                                  <xsl:if test="input/param">
                                      <div class="card-body" style="padding:0px">
                                        <table class="table">
                                          <thead><tr><th class="col-md-1">Parameter</th><th>Description</th></tr></thead>
                                          <tbody>
                                            <xsl:for-each select="input/param">
                                              <xsl:choose>
                                                <xsl:when test="@lookup">
                                                  <tr><td><a><xsl:attribute name="href">../core.html?page=<xsl:value-of select="@lookup"/></xsl:attribute><xsl:value-of select="@name"/></a></td><td><xsl:apply-templates select="."/></td></tr>
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
                                  </xsl:if>
                                </div>

                                <div class="docs-content" style="margin:1em .5em 1em .5em;">
                                  <xsl:for-each select="description">
                                    <xsl:apply-templates/>
                                    <xsl:text>&#xa;</xsl:text>
                                  </xsl:for-each>
                                  <xsl:choose>
                                    <xsl:when test="result/error">
                                      <div class="sub-header">Error Codes</div>
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
                                      <div class="sub-header">Result</div>
                                      <p><xsl:apply-templates select="result/."/></p>
                                    </xsl:when>
                                  </xsl:choose>
                                </div>
                              </div>
                            </td>
                          </tr>
                        </xsl:if>
                        <xsl:text>&#xa;</xsl:text>
                      </xsl:for-each>
                    </tbody>
                  </table>
                </xsl:if>

                <xsl:if test="/book/methods/method">
                  <h3>Methods</h3>
                  <p>The following methods are currently supported:</p>
                  <table class="table">
                    <tbody>
                      <xsl:for-each select="/book/methods/method">
                        <tr data-bs-toggle="collapse" data-bs-target="_" role="button" style="scroll-margin:3em;">
                          <xsl:attribute name="data-bs-target">#tmx-<xsl:value-of select="name"/></xsl:attribute>
                          <th class="col-md-1 text-primary"><xsl:value-of select="name"/></th>
                          <td><xsl:attribute name="id">tm-<xsl:value-of select="name"/></xsl:attribute><xsl:apply-templates select="comment"/></td>
                        </tr>
                        <tr>
                          <td colspan="2" class="hiddenRow">
                            <div id="_" class="accordion-body collapse">
                              <xsl:attribute name="id">tmx-<xsl:value-of select="name"/></xsl:attribute>
                              <div class="card" style="border-radius: 0;">
                                <div class="card-header action-header" style="border-radius: 0;"><samp><xsl:value-of select="prototype"/></samp></div>
                                <xsl:if test="input/param">
                                  <div class="card-body" style="padding:0px">
                                    <table class="table">
                                      <thead><tr><th class="col-md-1">Parameter</th><th>Description</th></tr></thead>
                                      <tbody>
                                        <xsl:for-each select="input/param">
                                          <xsl:choose>
                                            <xsl:when test="@lookup">
                                              <tr><td><a><xsl:attribute name="href">?page=<xsl:value-of select="@lookup"/></xsl:attribute><xsl:value-of select="@name"/></a></td><td><xsl:apply-templates select="."/></td></tr>
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
                                </xsl:if>
                              </div>

                              <div class="docs-content" style="margin:1em .5em 1em .5em;">
                                <xsl:for-each select="description">
                                  <xsl:apply-templates/>
                                  <xsl:text>&#xa;</xsl:text>
                                </xsl:for-each>

                                <xsl:choose>
                                  <xsl:when test="result/error">
                                    <div class="sub-header">Error Codes</div>
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
                                    <div class="sub-header">Result</div>
                                    <p><xsl:apply-templates select="result/."/></p>
                                  </xsl:when>
                                </xsl:choose>
                              </div>
                            </div>
                          </td>
                        </tr>
                        <xsl:text>&#xa;</xsl:text>
                      </xsl:for-each>
                    </tbody>
                  </table>
                </xsl:if>
                <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> class documentation © <xsl:value-of select="/book/info/copyright"/></div>
              </div>

              <!-- TYPES -->
              <xsl:for-each select="types/constants">
                <div class="docs-content" style="display:none;">
                  <xsl:attribute name="id"><xsl:value-of select="@lookup"/></xsl:attribute>
                  <h1><xsl:value-of select="@lookup"/> Type</h1>
                  <p class="lead"><xsl:apply-templates select="@comment"/></p>
                  <table class="table">
                    <thead><tr><th class="col-md-1">Name</th><th>Description</th></tr></thead>
                    <tbody>
                      <xsl:for-each select="const">
                        <tr><th><xsl:value-of select="../@lookup"/>::<xsl:value-of select="@name"/></th><td><xsl:apply-templates select="."/></td></tr>
                        <xsl:text>&#xa;</xsl:text>
                      </xsl:for-each>
                    </tbody>
                  </table>
                  <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> module documentation © <xsl:value-of select="/book/info/copyright"/></div>
                </div>
                <xsl:text>&#xa;</xsl:text>
              </xsl:for-each> <!-- End of type scan -->

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
                  <div class="footer copyright text-right"><xsl:value-of select="/book/info/name"/> class documentation © <xsl:value-of select="/book/info/copyright"/></div>
                </div>
                <xsl:text>&#xa;</xsl:text>
              </xsl:for-each> <!-- End of struct scan -->

            </div> <!-- End of core content -->

          </div> <!-- row -->
        </div> <!-- container -->

        <script type="text/javascript" src="../../js/bootstrap.bundle.min.js"></script>
        <script type="text/javascript" src="../../js/base.js"></script>
        <script type="text/javascript"><xsl:text disable-output-escaping="yes">
const ready = fn => document.readyState !== 'loading' ? fn() : document.addEventListener('DOMContentLoaded', fn);

   var xslt = false;
   var url = window.location.pathname;
   var filename = url.substring(url.lastIndexOf('/')+1);
   if (filename.endsWith(".xml")) { // XSLT is being used to view this document
      filename = filename.substr(0, filename.length-3) + 'html';
      xslt = true;
   }

   // Auto-open the relevant sidebar branch

   var nl = document.querySelectorAll('li[class="api-ref"] > a[href="' + filename + '"]');
   if (nl.length > 0) {
      // Target the last matching element to avoid confusion with matching module names (e.g. Audio module == Audio class)
      var el = nl.item(nl.length-1)
      var parent = getParentNode(el, '[class="collapse"]');
      if (parent) new bootstrap.Collapse(parent, { show: true }); // Causes animation

      el.style.backgroundColor = '#d2f4ea';
   }

   var page = glParameters["page"];
   if (isEmpty(page)) page = glParameters["function"];
   var div = document.getElementById(page);
   if (!div) div = document.getElementById("default-page");
   if (div) div.style.display = "block";

   cancelAnimations();

ready(function(){
   // Initialise tooltips
   var tooltipTriggerList = [].slice.call(document.querySelectorAll('[data-bs-toggle="tooltip"]'))
   var tooltipList = tooltipTriggerList.map(function (tooltipTriggerEl) {
     return new bootstrap.Tooltip(tooltipTriggerEl)
   })

   // In XSLT mode, changing all HTML links to XML is helpful for navigation (if heavy handed)
   if (xslt) {
      var nl = document.querySelectorAll('a[href*=".html"]');
      nl.forEach((el) => {
         el.href = el.href.substr(0, el.href.length-5) + '.xml'
      })
   }
});

         </xsl:text></script>
      </body>
    </html>
  </xsl:template>
</xsl:stylesheet>
