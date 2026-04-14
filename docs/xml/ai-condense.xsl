<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:ai="http://kotuku.dev/ai-docs"
    exclude-result-prefixes="ai">

    <xsl:output method="xml" encoding="UTF-8" indent="yes"/>

    <!-- Root template - processes single file at a time -->
    <xsl:template match="/">
        <xsl:choose>
            <!-- If processing a single module -->
            <xsl:when test="book[info/type='module']">
                <xsl:apply-templates select="book"/>
            </xsl:when>
            <!-- If processing a single class -->
            <xsl:when test="book[info/type='class']">
                <xsl:apply-templates select="book" mode="class"/>
            </xsl:when>
            <!-- Default case - create wrapper -->
            <xsl:otherwise>
                <ai:api-docs version="1.0">
                    <xsl:attribute name="generated">
                        <xsl:value-of select="current-dateTime()"/>
                    </xsl:attribute>
                    <ai:modules>
                        <xsl:apply-templates select="book[info/type='module']"/>
                    </ai:modules>
                    <ai:classes>
                        <xsl:apply-templates select="book[info/type='class']" mode="class"/>
                    </ai:classes>
                </ai:api-docs>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>

    <!-- Module processing -->
    <xsl:template match="book[info/type='module']">
        <ai:m>
            <xsl:attribute name="n"><xsl:value-of select="info/name"/></xsl:attribute>
            <xsl:if test="info/prefix">
                <xsl:attribute name="p"><xsl:value-of select="info/prefix"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/version">
                <xsl:attribute name="v"><xsl:value-of select="info/version"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/status">
                <xsl:attribute name="st"><xsl:value-of select="info/status"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/classes/class">
                <xsl:attribute name="cl">
                    <xsl:for-each select="info/classes/class">
                        <xsl:value-of select="."/>
                        <xsl:if test="position() != last()">,</xsl:if>
                    </xsl:for-each>
                </xsl:attribute>
            </xsl:if>

            <!-- Functions -->
            <xsl:apply-templates select="function"/>

            <!-- Constants -->
            <xsl:apply-templates select="types/constants"/>

            <!-- Structs -->
            <xsl:apply-templates select="structs/struct"/>
        </ai:m>
    </xsl:template>

    <!-- Function processing -->
    <xsl:template match="function">
        <ai:f>
            <xsl:attribute name="n"><xsl:value-of select="name"/></xsl:attribute>
            <xsl:if test="comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(comment)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="prototype">
                <xsl:attribute name="p"><xsl:value-of select="normalize-space(prototype)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="result/@type">
                <xsl:attribute name="r"><xsl:value-of select="result/@type"/></xsl:attribute>
            </xsl:if>

            <!-- Input parameters -->
            <xsl:apply-templates select="input/param"/>

            <!-- Error codes -->
            <xsl:apply-templates select="result/error"/>
        </ai:f>
    </xsl:template>

    <!-- Constants group processing -->
    <xsl:template match="constants">
        <ai:c>
            <xsl:attribute name="l"><xsl:value-of select="@lookup"/></xsl:attribute>
            <xsl:if test="@comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(@comment)"/></xsl:attribute>
            </xsl:if>

            <xsl:apply-templates select="const"/>
        </ai:c>
    </xsl:template>

    <!-- Constant processing -->
    <xsl:template match="const">
        <ai:k>
            <xsl:attribute name="n"><xsl:value-of select="@name"/></xsl:attribute>
            <xsl:if test="@value">
                <xsl:attribute name="v"><xsl:value-of select="@value"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="text() and normalize-space(text()) != ''">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(.)"/></xsl:attribute>
            </xsl:if>
        </ai:k>
    </xsl:template>

    <!-- Struct processing -->
    <xsl:template match="struct">
        <ai:s>
            <xsl:attribute name="n"><xsl:value-of select="@name"/></xsl:attribute>
            <xsl:apply-templates select="field" mode="struct"/>
        </ai:s>
    </xsl:template>

    <!-- Struct field processing -->
    <xsl:template match="field" mode="struct">
        <ai:f>
            <xsl:attribute name="n"><xsl:value-of select="@name"/></xsl:attribute>
            <xsl:if test="@type">
                <xsl:attribute name="t"><xsl:value-of select="@type"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="@size">
                <xsl:attribute name="s"><xsl:value-of select="@size"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="text() and normalize-space(text()) != ''">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(.)"/></xsl:attribute>
            </xsl:if>
        </ai:f>
    </xsl:template>

    <!-- Class processing -->
    <xsl:template match="book[info/type='class']" mode="class">
        <ai:cl>
            <xsl:attribute name="n"><xsl:value-of select="info/name"/></xsl:attribute>
            <xsl:if test="info/module">
                <xsl:attribute name="mod"><xsl:value-of select="info/module"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(info/comment)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/category">
                <xsl:attribute name="cat"><xsl:value-of select="info/category"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/id">
                <xsl:attribute name="id"><xsl:value-of select="info/id"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="info/version">
                <xsl:attribute name="v"><xsl:value-of select="info/version"/></xsl:attribute>
            </xsl:if>

            <!-- Actions -->
            <xsl:apply-templates select="actions/action"/>

            <!-- Methods -->
            <xsl:apply-templates select="methods/method"/>

            <!-- Fields -->
            <xsl:apply-templates select="fields/field"/>
        </ai:cl>
    </xsl:template>

    <!-- Action processing -->
    <xsl:template match="action">
        <ai:a>
            <xsl:attribute name="n"><xsl:value-of select="name"/></xsl:attribute>
            <xsl:if test="comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(comment)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="prototype">
                <xsl:attribute name="p"><xsl:value-of select="normalize-space(prototype)"/></xsl:attribute>
            </xsl:if>

            <!-- Input parameters -->
            <xsl:apply-templates select="input/param"/>

            <!-- Error codes -->
            <xsl:apply-templates select="result/error"/>
        </ai:a>
    </xsl:template>

    <!-- Method processing -->
    <xsl:template match="method">
        <ai:m>
            <xsl:attribute name="n"><xsl:value-of select="name"/></xsl:attribute>
            <xsl:if test="comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(comment)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="prototype">
                <xsl:attribute name="p"><xsl:value-of select="normalize-space(prototype)"/></xsl:attribute>
            </xsl:if>

            <!-- Input parameters -->
            <xsl:apply-templates select="input/param"/>

            <!-- Error codes -->
            <xsl:apply-templates select="result/error"/>
        </ai:m>
    </xsl:template>

    <!-- Field processing -->
    <xsl:template match="field">
        <ai:f>
            <xsl:attribute name="n"><xsl:value-of select="name"/></xsl:attribute>
            <xsl:if test="comment">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(comment)"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="type">
                <xsl:attribute name="t"><xsl:value-of select="type"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="access">
                <xsl:attribute name="a"><xsl:value-of select="access/@read"/><xsl:value-of select="access/@write"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="@lookup">
                <xsl:attribute name="l"><xsl:value-of select="@lookup"/></xsl:attribute>
            </xsl:if>
        </ai:f>
    </xsl:template>

    <!-- Parameter processing -->
    <xsl:template match="param">
        <ai:i>
            <xsl:attribute name="n"><xsl:value-of select="@name"/></xsl:attribute>
            <xsl:if test="@type">
                <xsl:attribute name="t"><xsl:value-of select="@type"/></xsl:attribute>
            </xsl:if>
            <xsl:if test="text() and normalize-space(text()) != ''">
                <xsl:attribute name="c"><xsl:value-of select="normalize-space(.)"/></xsl:attribute>
            </xsl:if>
        </ai:i>
    </xsl:template>

    <!-- Error processing -->
    <xsl:template match="error">
        <ai:e>
            <xsl:attribute name="c"><xsl:value-of select="@code"/></xsl:attribute>
            <xsl:if test="text() and normalize-space(text()) != ''">
            </xsl:if>
        </ai:e>
    </xsl:template>

</xsl:stylesheet>