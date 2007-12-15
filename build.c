/** \ingroup rpmcli
 * Parse spec file and build package.
 */

#include "system.h"

#include <rpm/rpmcli.h>
#include <rpm/rpmbuild.h>

#include <rpm/rpmps.h>
#include <rpm/rpmte.h>
#include <rpm/rpmts.h>
#include <rpm/rpmfileutil.h>

#include "build.h"
#include <rpm/rpmlog.h>
#include "debug.h"

/**
 */
static int checkSpec(rpmts ts, Header h)
{
    rpmps ps;
    int rc;

    if (!headerIsEntry(h, RPMTAG_REQUIRENAME)
     && !headerIsEntry(h, RPMTAG_CONFLICTNAME))
	return 0;

    rc = rpmtsAddInstallElement(ts, h, NULL, 0, NULL);

    rc = rpmtsCheck(ts);

    ps = rpmtsProblems(ts);
    if (rc == 0 && rpmpsNumProblems(ps) > 0) {
	rpmlog(RPMLOG_ERR, _("Failed build dependencies:\n"));
	rpmpsPrint(NULL, ps);
	rc = 1;
    }
    ps = rpmpsFree(ps);

    /* XXX nuke the added package. */
    rpmtsClean(ts);

    return rc;
}

/**
 */
static int isSpecFile(const char * specfile)
{
    char buf[256];
    const char * s;
    FD_t fd;
    int count;
    int checking;

    fd = Fopen(specfile, "r.ufdio");
    if (fd == NULL || Ferror(fd)) {
	rpmlog(RPMLOG_ERR, _("Unable to open spec file %s: %s\n"),
		specfile, Fstrerror(fd));
	return 0;
    }
    count = Fread(buf, sizeof(buf[0]), sizeof(buf), fd);
    (void) Fclose(fd);

    checking = 1;
    for (s = buf; count--; s++) {
	switch (*s) {
	case '\r':
	case '\n':
	    checking = 1;
	    break;
	case ':':
	    checking = 0;
	    break;
	default:
#if 0
	    if (checking && !(isprint(*s) || isspace(*s))) return 0;
	    break;
#else
	    if (checking && !(isprint(*s) || isspace(*s)) && *(unsigned char *)s < 32) return 0;
	    break;
#endif
	}
    }
    return 1;
}

/**
 */
static int buildForTarget(rpmts ts, const char * arg, BTA_t ba)
{
    const char * passPhrase = ba->passPhrase;
    const char * cookie = ba->cookie;
    int buildAmount = ba->buildAmount;
    const char * buildRootURL = NULL;
    const char * specFile;
    const char * specURL;
    int specut;
    char buf[BUFSIZ];
    rpmSpec spec = NULL;
    int rc;

#ifndef	DYING
    rpmSetTables(RPM_MACHTABLE_BUILDARCH, RPM_MACHTABLE_BUILDOS);
#endif

    if (ba->buildRootOverride)
	buildRootURL = rpmGenPath(NULL, ba->buildRootOverride, NULL);

    /* FIX: static zcmds heartburn */
    if (ba->buildMode == 't') {
	FILE *fp;
	char * specDir;
	char * tmpSpecFile;
	char * cmd, * s;
	rpmCompressedMagic res = COMPRESSED_OTHER;
	static const char *zcmds[] =
		{ "cat", "gunzip", "bunzip2", "cat" };

	specDir = rpmGetPath("%{_specdir}", NULL);

	tmpSpecFile = (char *) rpmGetPath("%{_specdir}/", "rpm-spec.XXXXXX", NULL);
#if defined(HAVE_MKSTEMP)
	(void) close(mkstemp(tmpSpecFile));
#else
	(void) mktemp(tmpSpecFile);
#endif

	(void) isCompressed(arg, &res);

	cmd = alloca(strlen(arg) + 50 + strlen(tmpSpecFile));
	sprintf(cmd, "%s < '%s' | tar xOvf - Specfile 2>&1 > '%s'",
			zcmds[res & 0x3], arg, tmpSpecFile);
	if (!(fp = popen(cmd, "r"))) {
	    rpmlog(RPMLOG_ERR, _("Failed to open tar pipe: %m\n"));
	    specDir = _free(specDir);
	    tmpSpecFile = _free(tmpSpecFile);
	    return 1;
	}
	if ((!fgets(buf, sizeof(buf) - 1, fp)) || !strchr(buf, '/')) {
	    /* Try again */
	    (void) pclose(fp);

	    sprintf(cmd, "%s < '%s' | tar xOvf - --wildcards \\*.spec 2>&1 > '%s'",
		    zcmds[res & 0x3], arg, tmpSpecFile);
	    if (!(fp = popen(cmd, "r"))) {
		rpmlog(RPMLOG_ERR, _("Failed to open tar pipe: %m\n"));
		specDir = _free(specDir);
		tmpSpecFile = _free(tmpSpecFile);
		return 1;
	    }
	    if (!fgets(buf, sizeof(buf) - 1, fp)) {
		/* Give up */
		rpmlog(RPMLOG_ERR, _("Failed to read spec file from %s\n"),
			arg);
		(void) unlink(tmpSpecFile);
		specDir = _free(specDir);
		tmpSpecFile = _free(tmpSpecFile);
	    	return 1;
	    }
	}
	(void) pclose(fp);

	cmd = s = buf;
	while (*cmd != '\0') {
	    if (*cmd == '/') s = cmd + 1;
	    cmd++;
	}

	cmd = s;

	/* remove trailing \n */
	s = cmd + strlen(cmd) - 1;
	*s = '\0';

	specURL = s = alloca(strlen(specDir) + strlen(cmd) + 5);
	sprintf(s, "%s/%s", specDir, cmd);
	res = rename(tmpSpecFile, s);
	specDir = _free(specDir);
	
	if (res) {
	    rpmlog(RPMLOG_ERR, _("Failed to rename %s to %s: %m\n"),
			tmpSpecFile, s);
	    (void) unlink(tmpSpecFile);
	    tmpSpecFile = _free(tmpSpecFile);
	    return 1;
	}
	tmpSpecFile = _free(tmpSpecFile);

	/* Make the directory which contains the tarball the source 
	   directory for this run */

	if (*arg != '/') {
	    if (!getcwd(buf, BUFSIZ)) {
		rpmlog(RPMLOG_ERR, "getcwd failed: %m\n");
		return 1;
	    }
	    strcat(buf, "/");
	    strcat(buf, arg);
	} else 
	    strcpy(buf, arg);

	cmd = buf + strlen(buf) - 1;
	while (*cmd != '/') cmd--;
	*cmd = '\0';

	addMacro(NULL, "_sourcedir", NULL, buf, RMIL_TARBALL);
    } else {
	specURL = arg;
    }

    specut = urlPath(specURL, &specFile);
    if (*specFile != '/') {
	char *s = alloca(BUFSIZ);
	if (!getcwd(s, BUFSIZ)) {
	    rpmlog(RPMLOG_ERR, "getcwd failed: %m\n");
	    rc = 1;
	    goto exit;
	}
	strcat(s, "/");
	strcat(s, arg);
	specURL = s;
    }

    if (specut != URL_IS_DASH) {
	struct stat st;
	if (stat(specURL, &st) < 0) {
	    rpmlog(RPMLOG_ERR, _("failed to stat %s: %m\n"), specURL);
	    rc = 1;
	    goto exit;
	}
	if (! S_ISREG(st.st_mode)) {
	    rpmlog(RPMLOG_ERR, _("File %s is not a regular file.\n"),
		specURL);
	    rc = 1;
	    goto exit;
	}

	/* Try to verify that the file is actually a specfile */
	if (!isSpecFile(specURL)) {
	    rpmlog(RPMLOG_ERR,
		_("File %s does not appear to be a specfile.\n"), specURL);
	    rc = 1;
	    goto exit;
	}
    }
    
    /* Parse the spec file */
#define	_anyarch(_f)	\
(((_f)&(RPMBUILD_PREP|RPMBUILD_BUILD|RPMBUILD_INSTALL|RPMBUILD_PACKAGEBINARY)) == 0)
    if (parseSpec(ts, specURL, ba->rootdir, buildRootURL, 0, passPhrase,
		cookie, _anyarch(buildAmount), ba->force))
    {
	rc = 1;
	goto exit;
    }
#undef	_anyarch
    if ((spec = rpmtsSetSpec(ts, NULL)) == NULL) {
	rc = 1;
	goto exit;
    }

    /* Assemble source header from parsed components */
    initSourceHeader(spec);

    /* Check build prerequisites */
    if (!ba->noDeps && checkSpec(ts, spec->sourceHeader)) {
	rc = 1;
	goto exit;
    }

    if (buildSpec(ts, spec, buildAmount, ba->noBuild)) {
	rc = 1;
	goto exit;
    }
    
    if (ba->buildMode == 't')
	(void) unlink(specURL);
    rc = 0;

exit:
    spec = freeSpec(spec);
    buildRootURL = _free(buildRootURL);
    return rc;
}

int build(rpmts ts, const char * arg, BTA_t ba, const char * rcfile)
{
    char *t, *te;
    int rc = 0;
    char * targets = ba->targets;
#define	buildCleanMask	(RPMBUILD_RMSOURCE|RPMBUILD_RMSPEC)
    int cleanFlags = ba->buildAmount & buildCleanMask;
    rpmVSFlags vsflags, ovsflags;

    vsflags = rpmExpandNumeric("%{_vsflags_build}");
    if (ba->qva_flags & VERIFY_DIGEST)
	vsflags |= _RPMVSF_NODIGESTS;
    if (ba->qva_flags & VERIFY_SIGNATURE)
	vsflags |= _RPMVSF_NOSIGNATURES;
    if (ba->qva_flags & VERIFY_HDRCHK)
	vsflags |= RPMVSF_NOHDRCHK;
    ovsflags = rpmtsSetVSFlags(ts, vsflags);

    if (targets == NULL) {
	rc =  buildForTarget(ts, arg, ba);
	goto exit;
    }

    /* parse up the build operators */

    printf(_("Building target platforms: %s\n"), targets);

    ba->buildAmount &= ~buildCleanMask;
    for (t = targets; *t != '\0'; t = te) {
	char *target;
	if ((te = strchr(t, ',')) == NULL)
	    te = t + strlen(t);
	target = alloca(te-t+1);
	strncpy(target, t, (te-t));
	target[te-t] = '\0';
	if (*te != '\0')
	    te++;
	else	/* XXX Perform clean-up after last target build. */
	    ba->buildAmount |= cleanFlags;

	printf(_("Building for target %s\n"), target);

	/* Read in configuration for target. */
	rpmFreeMacros(NULL);
	rpmFreeRpmrc();
	(void) rpmReadConfigFiles(rcfile, target);
	rc = buildForTarget(ts, arg, ba);
	if (rc)
	    break;
    }

exit:
    vsflags = rpmtsSetVSFlags(ts, ovsflags);
    /* Restore original configuration. */
    rpmFreeMacros(NULL);
    rpmFreeRpmrc();
    (void) rpmReadConfigFiles(rcfile, NULL);

    return rc;
}
