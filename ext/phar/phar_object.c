/*
  +----------------------------------------------------------------------+
  | phar php single-file executable PHP extension                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2005-2007 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Gregory Beaver <cellog@php.net>                             |
  |          Marcus Boerger <helly@php.net>                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "phar_internal.h"
#include "func_interceptors.h"

static zend_class_entry *phar_ce_archive;
static zend_class_entry *phar_ce_data;
static zend_class_entry *phar_ce_PharException;

#if HAVE_SPL
static zend_class_entry *phar_ce_entry;
#endif

static int phar_get_extract_list(void *pDest, int num_args, va_list args, zend_hash_key *hash_key) /* {{{ */
{
	zval *return_value = va_arg(args, zval*);

	add_assoc_string_ex(return_value, *(char**)&hash_key->arKey, hash_key->nKeyLength, (char*)pDest, 1);
	
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{{ proto array Phar::getExtractList()
 * Return array of extract list
 */
PHP_METHOD(Phar, getExtractList)
{
	array_init(return_value);

	phar_request_initialize(TSRMLS_C);
	zend_hash_apply_with_arguments(&PHAR_G(phar_plain_map), phar_get_extract_list, 1, return_value);
}
/* }}} */

static int phar_file_type(HashTable *mimes, char *file, char **mime_type TSRMLS_DC) /* {{{ */
{
	char *ext;
	phar_mime_type *mime;
	ext = strrchr(file, '.');
	if (!ext) {
		*mime_type = "text/plain";
		/* no file extension = assume text/plain */
		return PHAR_MIME_OTHER;
	}
	++ext;
	if (SUCCESS != zend_hash_find(mimes, ext, strlen(ext), (void **) &mime)) {
		*mime_type = "application/octet-stream";
		return PHAR_MIME_OTHER;
	}
	*mime_type = mime->mime;
	return mime->type;
}
/* }}} */

static void phar_mung_server_vars(char *fname, char *entry, int entry_len, char *basename, int basename_len, char *request_uri, int request_uri_len TSRMLS_DC) /* {{{ */
{
	zval **_SERVER, **stuff;
	char *path_info;

	/* "tweak" $_SERVER variables requested in earlier call to Phar::mungServer() */
	if (SUCCESS != zend_hash_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER"), (void **) &_SERVER)) {
		return;
	}

	/* PATH_INFO and PATH_TRANSLATED should always be munged */
	if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "PATH_INFO", sizeof("PATH_INFO"), (void **) &stuff)) { 
		int code; 
		zval *temp; 
		char newname[] = "PHAR_PATH_INFO";

		path_info = Z_STRVAL_PP(stuff);
		code = Z_STRLEN_PP(stuff);
		if (Z_STRLEN_PP(stuff) > entry_len && !memcmp(Z_STRVAL_PP(stuff), entry, entry_len)) {
			ZVAL_STRINGL(*stuff, Z_STRVAL_PP(stuff) + entry_len, request_uri_len, 1);

			MAKE_STD_ZVAL(temp); 
			ZVAL_STRINGL(temp, path_info, code, 0);
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL);
		}
	}
	if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "PATH_TRANSLATED", sizeof("PATH_TRANSLATED"), (void **) &stuff)) { 
		int code; 
		zval *temp; 
		char newname[] = "PHAR_PATH_TRANSLATED";

		path_info = Z_STRVAL_PP(stuff); 
		code = Z_STRLEN_PP(stuff); 
		Z_STRLEN_PP(stuff) = spprintf(&(Z_STRVAL_PP(stuff)), 4096, "phar://%s%s", fname, entry);

		MAKE_STD_ZVAL(temp);
		ZVAL_STRINGL(temp, path_info, code, 0);
		zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL); 
	}
	if (!PHAR_GLOBALS->phar_SERVER_mung_list.arBuckets || !zend_hash_num_elements(&(PHAR_GLOBALS->phar_SERVER_mung_list))) {
		return;
	}
	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "REQUEST_URI", sizeof("REQUEST_URI")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "REQUEST_URI", sizeof("REQUEST_URI"), (void **) &stuff)) { 
			int code;
			zval *temp;
			char newname[] = "PHAR_REQUEST_URI";

			path_info = Z_STRVAL_PP(stuff);
			code = Z_STRLEN_PP(stuff);
			if (Z_STRLEN_PP(stuff) > basename_len && !memcmp(Z_STRVAL_PP(stuff), basename, basename_len)) {
				ZVAL_STRINGL(*stuff, Z_STRVAL_PP(stuff) + basename_len, Z_STRLEN_PP(stuff) - basename_len, 1);

				MAKE_STD_ZVAL(temp);
				ZVAL_STRINGL(temp, path_info, code, 0);
				zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL);
			}
		}
	}
	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "PHP_SELF", sizeof("PHP_SELF")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "PHP_SELF", sizeof("PHP_SELF"), (void **) &stuff)) { 
			int code;
			zval *temp;
			char newname[] = "PHAR_PHP_SELF";

			path_info = Z_STRVAL_PP(stuff);
			code = Z_STRLEN_PP(stuff);
			if (Z_STRLEN_PP(stuff) > basename_len && !memcmp(Z_STRVAL_PP(stuff), basename, basename_len)) {
				ZVAL_STRINGL(*stuff, Z_STRVAL_PP(stuff) + basename_len, Z_STRLEN_PP(stuff) - basename_len, 1);

				MAKE_STD_ZVAL(temp);
				ZVAL_STRINGL(temp, path_info, code, 0);
				zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL);
			}
		}
	}

	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "SCRIPT_NAME", sizeof("SCRIPT_NAME"), (void **) &stuff)) { 
			int code; 
			zval *temp; 
			char newname[] = "PHAR_SCRIPT_NAME";
							
			path_info = Z_STRVAL_PP(stuff); 
			code = Z_STRLEN_PP(stuff); 
			ZVAL_STRINGL(*stuff, entry, entry_len, 1);
						
			MAKE_STD_ZVAL(temp);
			ZVAL_STRINGL(temp, path_info, code, 0);
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL); 
		} 
	}

	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME"), (void **) &stuff)) {
			int code; 
			zval *temp; 
			char newname[] = "PHAR_SCRIPT_FILENAME";

			path_info = Z_STRVAL_PP(stuff); 
			code = Z_STRLEN_PP(stuff); 
			Z_STRLEN_PP(stuff) = spprintf(&(Z_STRVAL_PP(stuff)), 4096, "phar://%s%s", fname, entry);

			MAKE_STD_ZVAL(temp);
			ZVAL_STRINGL(temp, path_info, code, 0);
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, sizeof(newname), (void *) &temp, sizeof(zval **), NULL); 
		}
	}
}
/* }}} */

static int phar_file_action(phar_entry_data *phar, char *mime_type, int code, char *entry, int entry_len, char *arch, int arch_len, char *basename, int basename_len, char *ru, int ru_len TSRMLS_DC) /* {{{ */
{
	char *name = NULL, buf[8192], *cwd;
	zend_syntax_highlighter_ini syntax_highlighter_ini;
	sapi_header_line ctr = {0};
	size_t got;
	int dummy = 1, name_len, ret;
	zend_file_handle file_handle;
	zend_op_array *new_op_array;
	zval *result = NULL;

	switch (code) {
		case PHAR_MIME_PHPS:
			efree(basename);
			/* highlight source */
			if (entry[0] == '/') {
				name_len = spprintf(&name, 4096, "phar://%s%s", arch, entry);
			} else {
				name_len = spprintf(&name, 4096, "phar://%s/%s", arch, entry);
			}
			php_get_highlight_struct(&syntax_highlighter_ini);

			highlight_file(name, &syntax_highlighter_ini TSRMLS_CC);

			phar_entry_delref(phar TSRMLS_CC);
			efree(name);
#ifdef PHP_WIN32
			efree(arch);
#endif
			zend_bailout();
		case PHAR_MIME_OTHER:
			/* send headers, output file contents */
			efree(basename);
			ctr.line_len = spprintf(&(ctr.line), 0, "Content-type: %s", mime_type);
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			efree(ctr.line);
			ctr.line_len = spprintf(&(ctr.line), 0, "Content-length: %d", phar->internal_file->uncompressed_filesize);
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			efree(ctr.line);
			if (FAILURE == sapi_send_headers(TSRMLS_C)) {
				phar_entry_delref(phar TSRMLS_CC);
				zend_bailout();
			}

			/* prepare to output  */
			if (!phar_get_efp(phar->internal_file, 1 TSRMLS_CC)) {
				char *error;
				if (!phar_open_jit(phar->phar, phar->internal_file, phar->phar->fp, &error, 0 TSRMLS_CC)) {
					if (error) {
						zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
						efree(error);
					}
					return -1;
				}
				phar->fp = phar_get_efp(phar->internal_file, 1 TSRMLS_CC);
				phar->zero = phar->internal_file->offset;
			}
			phar_seek_efp(phar->internal_file, 0, SEEK_SET, 0, 1 TSRMLS_CC);
			do {
				got = php_stream_read(phar->fp, buf, MIN(8192, phar->internal_file->uncompressed_filesize - phar->position));
				PHPWRITE(buf, got);
				phar->position = php_stream_tell(phar->fp) - phar->zero;
				if (phar->position == (off_t) phar->internal_file->uncompressed_filesize) {
					break;
				}
			} while (1);

			phar_entry_delref(phar TSRMLS_CC);
			zend_bailout();
		case PHAR_MIME_PHP:
			if (basename) {
				phar_mung_server_vars(arch, entry, entry_len, basename, basename_len, ru, ru_len TSRMLS_CC);
				efree(basename);
			}
			phar_entry_delref(phar TSRMLS_CC);
			if (entry[0] == '/') {
				name_len = spprintf(&name, 4096, "phar://%s%s", arch, entry);
			} else {
				name_len = spprintf(&name, 4096, "phar://%s/%s", arch, entry);
			}

			ret = php_stream_open_for_zend_ex(name, &file_handle, ENFORCE_SAFE_MODE|USE_PATH|STREAM_OPEN_FOR_INCLUDE TSRMLS_CC);
			
			if (ret != SUCCESS) {
				efree(name);
				return -1;
			}
			PHAR_G(cwd) = NULL;
			PHAR_G(cwd_len) = 0;
			if (zend_hash_add(&EG(included_files), file_handle.opened_path, strlen(file_handle.opened_path)+1, (void *)&dummy, sizeof(int), NULL)==SUCCESS) {
				if ((cwd = strrchr(entry, '/'))) {
					if (entry == cwd) {
						/* root directory */
						PHAR_G(cwd_len) = 0;
						PHAR_G(cwd) = NULL;
					} else if (entry[0] == '/') {
						PHAR_G(cwd_len) = cwd - (entry + 1);
						PHAR_G(cwd) = estrndup(entry + 1, PHAR_G(cwd_len));
					} else {
						PHAR_G(cwd_len) = cwd - entry;
						PHAR_G(cwd) = estrndup(entry, PHAR_G(cwd_len));
					}
				}
				new_op_array = zend_compile_file(&file_handle, ZEND_REQUIRE TSRMLS_CC);
				zend_destroy_file_handle(&file_handle TSRMLS_CC);
			} else {
				new_op_array = NULL;
#if PHP_VERSION_ID >= 50300
				zend_file_handle_dtor(&file_handle TSRMLS_CC);
#else
				zend_file_handle_dtor(&file_handle);
#endif
			}
#ifdef PHP_WIN32
			efree(arch);
#endif
			if (new_op_array) {
				EG(return_value_ptr_ptr) = &result;
				EG(active_op_array) = new_op_array;

				zend_try {
					zend_execute(new_op_array TSRMLS_CC);
					destroy_op_array(new_op_array TSRMLS_CC);
					efree(new_op_array);
					if (!EG(exception)) {
						if (EG(return_value_ptr_ptr)) {
							zval_ptr_dtor(EG(return_value_ptr_ptr));
						}
					}
				} zend_catch {
				} zend_end_try();
				if (PHAR_G(cwd)) {
					efree(PHAR_G(cwd));
					PHAR_G(cwd) = NULL;
					PHAR_G(cwd_len) = 0;
				}
				efree(name);
				zend_bailout();
			}
			return PHAR_MIME_PHP;
	}
	return -1;
}
/* }}} */

static void phar_do_403(char *entry, int entry_len TSRMLS_DC) /* {{{ */
{
	sapi_header_line ctr = {0};

	ctr.response_code = 403;
	ctr.line_len = sizeof("HTTP/1.0 403 Access Denied");
	ctr.line = "HTTP/1.0 403 Access Denied";
	sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
	sapi_send_headers(TSRMLS_C);
	PHPWRITE("<html>\n <head>\n  <title>Access Denied</title>\n </head>\n <body>\n  <h1>403 - File ", sizeof("<html>\n <head>\n  <title>Access Denied</title>\n </head>\n <body>\n  <h1>403 - File ") - 1);
	PHPWRITE(entry, entry_len);
	PHPWRITE(" Access Denied</h1>\n </body>\n</html>", sizeof(" Access Denied</h1>\n </body>\n</html>") - 1);
}
/* }}} */

static void phar_do_404(char *fname, int fname_len, char *f404, int f404_len, char *entry, int entry_len TSRMLS_DC) /* {{{ */
{
	int hi;
	phar_entry_data *phar;
	char *error;
	if (f404_len) {
		if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, f404, f404_len, "r", 0, &error TSRMLS_CC)) {
			if (error) {
				efree(error);
			}
			goto nofile;
		}
		hi = phar_file_action(phar, "text/html", PHAR_MIME_PHP, f404, f404_len, fname, fname_len, NULL, 0, NULL, 0 TSRMLS_CC);
	} else {
		sapi_header_line ctr = {0};
nofile:
		ctr.response_code = 404;
		ctr.line_len = sizeof("HTTP/1.0 404 Not Found")+1;
		ctr.line = "HTTP/1.0 404 Not Found";
		sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
		sapi_send_headers(TSRMLS_C);
		PHPWRITE("<html>\n <head>\n  <title>File Not Found</title>\n </head>\n <body>\n  <h1>404 - File ", sizeof("<html>\n <head>\n  <title>File Not Found</title>\n </head>\n <body>\n  <h1>404 - File ") - 1);
		PHPWRITE(entry, entry_len);
		PHPWRITE(" Not Found</h1>\n </body>\n</html>",  sizeof(" Not Found</h1>\n </body>\n</html>") - 1);
	}
}
/* }}} */

/* post-process REQUEST_URI and retrieve the actual request URI.  This is for
   cases like http://localhost/blah.phar/path/to/file.php/extra/stuff
   which calls "blah.phar" file "path/to/file.php" with PATH_INFO "/extra/stuff" */
static void phar_postprocess_ru_web(char *fname, int fname_len, char **entry, int *entry_len, char **ru, int *ru_len TSRMLS_DC) /* {{{ */
{
	char *e = *entry + 1, *u = NULL, *u1 = NULL, *saveu = NULL;
	int e_len = *entry_len - 1, u_len = 0;
	phar_archive_data **pphar;

	/* we already know we can retrieve the phar if we reach here */
	zend_hash_find(&(PHAR_GLOBALS->phar_fname_map), fname, fname_len, (void **) &pphar);

	do {
		if (zend_hash_exists(&((*pphar)->manifest), e, e_len)) {
			if (u) {
				u[0] = '/';
				*ru = estrndup(u, u_len+1);
				++u_len;
				u[0] = '\0';
			} else {
				*ru = NULL;
			}
			*ru_len = u_len;
			*entry_len = e_len + 1;
			return;
		}
		if (u) {
			u1 = strrchr(e, '/');
			u[0] = '/';
			saveu = u;
			e_len += u_len + 1;
			u = u1;
			if (!u) {
				return;
			}
		} else {
			u = strrchr(e, '/');
			if (!u) {
				if (saveu) {
					saveu[0] = '/';
				}
				return;
			}
		}
		u[0] = '\0';
		u_len = strlen(u + 1);
		e_len -= u_len + 1;
		if (e_len < 0) {
			if (saveu) {
				saveu[0] = '/';
			}
			return;
		}
	} while (1);
}
/* }}} */

/* {{{ proto void Phar::running([bool retphar = true])
 * return the name of the currently running phar archive.  If the optional parameter
 * is set to true, return the phar:// URL to the currently running phar
 */
PHP_METHOD(Phar, running)
{
	char *fname, *arch, *entry;
	int fname_len, arch_len, entry_len;
	zend_bool retphar = 1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &retphar) == FAILURE) {
		return;
	}

	fname = zend_get_executed_filename(TSRMLS_C);
	fname_len = strlen(fname);

	if (fname_len > 7 && !memcmp(fname, "phar://", 7) && SUCCESS == phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len, 2, 0 TSRMLS_CC)) {
		efree(entry);
		if (retphar) {
			RETVAL_STRINGL(fname, arch_len + 7, 1);
			efree(arch);
			return;
		} else {
			RETURN_STRINGL(arch, arch_len, 0);
		}
	}
	RETURN_STRINGL("", 0, 1);
}
/* }}} */

/* {{{ proto void Phar::mount(string pharpath, string externalfile)
 * mount an external file or path to a location within the phar.  This maps
 * an external file or directory to a location within the phar archive, allowing
 * reference to an external location as if it were within the phar archive.  This
 * is useful for writable temp files like databases
 */
PHP_METHOD(Phar, mount)
{
	char *fname, *arch, *entry, *path, *actual;
	int fname_len, arch_len, entry_len, path_len, actual_len;
	phar_archive_data **pphar;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &path, &path_len, &actual, &actual_len) == FAILURE) {
		return;
	}

	fname = zend_get_executed_filename(TSRMLS_C);
	fname_len = strlen(fname);

	if (fname_len > 7 && !memcmp(fname, "phar://", 7) && SUCCESS == phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len, 2, 0 TSRMLS_CC)) {
		efree(entry);
		entry = NULL;
		if (path_len > 7 && !memcmp(path, "phar://", 7)) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Can only mount internal paths within a phar archive, use a relative path instead of \"%s\"", path);
			efree(arch);
			return;
		}
carry_on2:
		if (SUCCESS != zend_hash_find(&(PHAR_GLOBALS->phar_fname_map), arch, arch_len, (void **)&pphar)) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "%s is not a phar archive, cannot mount", arch);
			efree(arch);
			return;
		}
carry_on:
		if (SUCCESS != phar_mount_entry(*pphar, actual, actual_len, path, path_len TSRMLS_CC)) {
			if (path && path == entry) {
				efree(entry);
			}
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Mounting of %s to %s within phar %s failed", path, actual, arch);
		}
		if (path && path == entry) {
			efree(entry);
		}
		efree(arch);
		return;
	} else if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_fname_map), fname, fname_len, (void **)&pphar)) {
		goto carry_on;
	} else if (SUCCESS == phar_split_fname(path, path_len, &arch, &arch_len, &entry, &entry_len, 2, 0 TSRMLS_CC)) {
		path = entry;
		path_len = entry_len;
		goto carry_on2;
	}
}
/* }}} */

/* {{{ proto void Phar::webPhar([string alias, [string index, [string f404, [array mimetypes, [callback rewrites]]]]])
 * mapPhar for web-based phars. Reads the currently executed file (a phar)
 * and registers its manifest. When executed in the CLI or CGI command-line sapi,
 * this works exactly like mapPhar().  When executed by a web-based sapi, this
 * reads $_SERVER['REQUEST_URI'] (the actual original value) and parses out the
 * intended internal file.
 */
PHP_METHOD(Phar, webPhar)
{
	HashTable mimetypes;
	phar_mime_type mime;
	zval *mimeoverride = NULL, *rewrite = NULL;
	char *alias = NULL, *error, *plain_map, *index_php, *f404 = NULL, *ru = NULL;
	int alias_len = 0, ret, f404_len = 0, free_pathinfo = 0, ru_len = 0;
	char *fname, *basename, *path_info, *mime_type, *entry, *pt;
	int fname_len, entry_len, code, index_php_len = 0, not_cgi;
	phar_entry_data *phar;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!s!saz", &alias, &alias_len, &index_php, &index_php_len, &f404, &f404_len, &mimeoverride, &rewrite) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	fname = zend_get_executed_filename(TSRMLS_C);
	fname_len = strlen(fname);
	if (zend_hash_num_elements(&(PHAR_GLOBALS->phar_plain_map))) {
		if((alias && 
		    zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), alias, alias_len+1, (void **)&plain_map) == SUCCESS)
		|| (zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), fname, fname_len+1, (void **)&plain_map) == SUCCESS)
		) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Cannot use Phar::webPhar() from an extracted phar archive, simply use the extracted files directly");
			return;
		}
	}
	if (phar_open_compiled_file(alias, alias_len, &error TSRMLS_CC) != SUCCESS) {
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
		return;
	}

	/* retrieve requested file within phar */
	if (!(SG(request_info).request_method && SG(request_info).request_uri && (!strcmp(SG(request_info).request_method, "GET") || !strcmp(SG(request_info).request_method, "POST")))) {
		return;
	}
#ifdef PHP_WIN32
	fname = estrndup(fname, fname_len);
	phar_unixify_path_separators(fname, fname_len);
#endif
	basename = strrchr(fname, '/');
	if (!basename) {
		basename = fname;
	} else {
		++basename;
	}

	if ((strlen(sapi_module.name) == sizeof("cgi-fcgi")-1 && !strncmp(sapi_module.name, "cgi-fcgi", sizeof("cgi-fcgi")-1))
		|| (strlen(sapi_module.name) == sizeof("cgi")-1 && !strncmp(sapi_module.name, "cgi", sizeof("cgi")-1))) {
		char *testit;

		testit = sapi_getenv("SCRIPT_NAME", sizeof("SCRIPT_NAME")-1 TSRMLS_CC);
		if (!(pt = strstr(testit, basename))) {
			return;
		}
		path_info = sapi_getenv("PATH_INFO", sizeof("PATH_INFO")-1 TSRMLS_CC);
		if (path_info) {
			entry = estrdup(path_info);
			entry_len = strlen(entry);
			spprintf(&path_info, 0, "%s%s", testit, path_info);
			free_pathinfo = 1;
		} else {
			path_info = testit;
			entry = estrndup("", 0);
			entry_len = 0;
		}
		pt = estrndup(testit, (pt - testit) + (fname_len - (basename - fname)));
		not_cgi = 0;
	} else {
		path_info = SG(request_info).request_uri;

		if (!(pt = strstr(path_info, basename))) {
			/* this can happen with rewrite rules - and we have no idea what to do then, so return */
			return;
		}
		entry_len = strlen(path_info);

		entry_len -= (pt - path_info) + (fname_len - (basename - fname));
		entry = estrndup(pt + (fname_len - (basename - fname)), entry_len);

		pt = estrndup(path_info, (pt - path_info) + (fname_len - (basename - fname)));
		not_cgi = 1;
	}

	if (rewrite) {
		zend_fcall_info fci;
		zend_fcall_info_cache fcc;
		zval *params, *retval_ptr, **zp[1];

		MAKE_STD_ZVAL(params);
		ZVAL_STRINGL(params, entry, entry_len, 1);
		zp[0] = &params;

#if PHP_VERSION_ID < 50300
		if (FAILURE == zend_fcall_info_init(rewrite, &fci, &fcc TSRMLS_CC)) {
#else
		if (FAILURE == zend_fcall_info_init(rewrite, 0, &fci, &fcc, NULL, NULL TSRMLS_CC)) {
#endif
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "phar error: invalid rewrite callback");
			if (free_pathinfo) {
				efree(path_info);
			}
			return;
		}

		fci.param_count = 1;
		fci.params = zp;
#if PHP_VERSION_ID < 50300
		++(params->refcount);
#else
		Z_ADDREF_P(params);
#endif
		fci.retval_ptr_ptr = &retval_ptr;

		if (FAILURE == zend_call_function(&fci, &fcc TSRMLS_CC)) {
			if (!EG(exception)) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "phar error: failed to call rewrite callback");
			}
			if (free_pathinfo) {
				efree(path_info);
			}
			return;
		}
		if (!fci.retval_ptr_ptr || !retval_ptr) {
			if (free_pathinfo) {
				efree(path_info);
			}
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "phar error: rewrite callback must return a string or false");
			return;
		}
		switch (Z_TYPE_P(retval_ptr)) {
			case IS_STRING :
				efree(entry);
				if (fci.retval_ptr_ptr != &retval_ptr) {
					entry = estrndup(Z_STRVAL_PP(fci.retval_ptr_ptr), Z_STRLEN_PP(fci.retval_ptr_ptr));
					entry_len = Z_STRLEN_PP(fci.retval_ptr_ptr);
				} else {
					entry = Z_STRVAL_P(retval_ptr);
					entry_len = Z_STRLEN_P(retval_ptr);
				}
				break;
			case IS_BOOL :
				phar_do_403(entry, entry_len TSRMLS_CC);
				if (free_pathinfo) {
					efree(path_info);
				}
				zend_bailout();
				return;
			default:
				efree(retval_ptr);
				if (free_pathinfo) {
					efree(path_info);
				}
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "phar error: rewrite callback must return a string or false");
				return;
		}
	}

	if (entry_len) {
		phar_postprocess_ru_web(fname, fname_len, &entry, &entry_len, &ru, &ru_len TSRMLS_CC);
	}
	if (!entry_len || (entry_len == 1 && entry[0] == '/')) {
		efree(entry);
		/* direct request */
		if (index_php_len) {
			entry = index_php;
			entry_len = index_php_len;
			if (entry[0] != '/') {
				spprintf(&entry, 0, "/%s", index_php);
				++entry_len;
			}
		} else {
			/* assume "index.php" is starting point */
			entry = estrndup("/index.php", sizeof("/index.php"));
			entry_len = sizeof("/index.php")-1;
		}
		if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, entry, entry_len, "r", 0, NULL TSRMLS_CC)) {
			phar_do_404(fname, fname_len, f404, f404_len, entry, entry_len TSRMLS_CC);
			if (free_pathinfo) {
				efree(path_info);
			}
			zend_bailout();
		} else {
			char *tmp, sa;
			sapi_header_line ctr = {0};
			ctr.response_code = 301;
			ctr.line_len = sizeof("HTTP/1.1 301 Moved Permanently")+1;
			ctr.line = "HTTP/1.1 301 Moved Permanently";
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);

			if (not_cgi) {
				tmp = strstr(path_info, basename) + fname_len;
				sa = *tmp;
				*tmp = '\0';
			}
			ctr.response_code = 0;
			if (path_info[strlen(path_info)-1] == '/') {
				ctr.line_len = spprintf(&(ctr.line), 4096, "Location: %s%s", path_info, entry + 1);
			} else {
				ctr.line_len = spprintf(&(ctr.line), 4096, "Location: %s%s", path_info, entry);
			}
			if (not_cgi) {
				*tmp = sa;
			}
			if (free_pathinfo) {
				efree(path_info);
			}
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			sapi_send_headers(TSRMLS_C);
			phar_entry_delref(phar TSRMLS_CC);
			efree(ctr.line);
			zend_bailout();
		}
	}

	if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, entry, entry_len, "r", 0, &error TSRMLS_CC)) {
		phar_do_404(fname, fname_len, f404, f404_len, entry, entry_len TSRMLS_CC);
#ifdef PHP_WIN32
		efree(fname);
#endif
		zend_bailout();
	}

	/* set up mime types */
	zend_hash_init(&mimetypes, sizeof(phar_mime_type *), zend_get_hash_value, NULL, 0);
#define PHAR_SET_MIME(mimetype, ret, fileext) \
		mime.mime = mimetype; \
		mime.len = sizeof((mimetype))+1; \
		mime.type = ret; \
		zend_hash_add(&mimetypes, fileext, sizeof(fileext)-1, (void *)&mime, sizeof(phar_mime_type), NULL); \

	PHAR_SET_MIME("text/html", PHAR_MIME_PHPS, "phps")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "c")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "cc")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "cpp")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "c++")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "dtd")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "h")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "log")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "rng")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "txt")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "xsd")
	PHAR_SET_MIME("", PHAR_MIME_PHP, "php")
	PHAR_SET_MIME("", PHAR_MIME_PHP, "inc")
	PHAR_SET_MIME("video/avi", PHAR_MIME_OTHER, "avi")
	PHAR_SET_MIME("image/bmp", PHAR_MIME_OTHER, "bmp")
	PHAR_SET_MIME("text/css", PHAR_MIME_OTHER, "css")
	PHAR_SET_MIME("image/gif", PHAR_MIME_OTHER, "gif")
	PHAR_SET_MIME("text/html", PHAR_MIME_OTHER, "htm")
	PHAR_SET_MIME("text/html", PHAR_MIME_OTHER, "html")
	PHAR_SET_MIME("text/html", PHAR_MIME_OTHER, "htmls")
	PHAR_SET_MIME("image/x-ico", PHAR_MIME_OTHER, "ico")
	PHAR_SET_MIME("image/jpeg", PHAR_MIME_OTHER, "jpe")
	PHAR_SET_MIME("image/jpeg", PHAR_MIME_OTHER, "jpg")
	PHAR_SET_MIME("image/jpeg", PHAR_MIME_OTHER, "jpeg")
	PHAR_SET_MIME("application/x-javascript", PHAR_MIME_OTHER, "js")
	PHAR_SET_MIME("audio/midi", PHAR_MIME_OTHER, "midi")
	PHAR_SET_MIME("audio/midi", PHAR_MIME_OTHER, "mid")
	PHAR_SET_MIME("audio/mod", PHAR_MIME_OTHER, "mod")
	PHAR_SET_MIME("movie/quicktime", PHAR_MIME_OTHER, "mov")
	PHAR_SET_MIME("audio/mp3", PHAR_MIME_OTHER, "mp3")
	PHAR_SET_MIME("video/mpeg", PHAR_MIME_OTHER, "mpg")
	PHAR_SET_MIME("video/mpeg", PHAR_MIME_OTHER, "mpeg")
	PHAR_SET_MIME("application/pdf", PHAR_MIME_OTHER, "pdf")
	PHAR_SET_MIME("image/png", PHAR_MIME_OTHER, "png")
	PHAR_SET_MIME("application/shockwave-flash", PHAR_MIME_OTHER, "swf")
	PHAR_SET_MIME("image/tiff", PHAR_MIME_OTHER, "tif")
	PHAR_SET_MIME("image/tiff", PHAR_MIME_OTHER, "tiff")
	PHAR_SET_MIME("audio/wav", PHAR_MIME_OTHER, "wav")
	PHAR_SET_MIME("image/xbm", PHAR_MIME_OTHER, "xbm")
	PHAR_SET_MIME("text/xml", PHAR_MIME_OTHER, "xml")

	/* set up user overrides */
#define PHAR_SET_USER_MIME(ret) \
		if (Z_TYPE_PP(val) == IS_LONG) { \
			mime.mime = ""; \
			mime.len = 0; \
		} else { \
			mime.mime = Z_STRVAL_PP(val); \
			mime.len = Z_STRLEN_PP(val); \
		} \
		mime.type = ret; \
		zend_hash_update(&mimetypes, key, keylen-1, (void *)&mime, sizeof(phar_mime_type), NULL);

	if (mimeoverride) {
		if (!zend_hash_num_elements(Z_ARRVAL_P(mimeoverride))) {
			goto no_mimes;
		}
		for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(mimeoverride)); SUCCESS == zend_hash_has_more_elements(Z_ARRVAL_P(mimeoverride)); zend_hash_move_forward(Z_ARRVAL_P(mimeoverride))) {
			zval **val;
			char *key;
			uint keylen;
			ulong intkey;
			if (HASH_KEY_IS_LONG == zend_hash_get_current_key_ex(Z_ARRVAL_P(mimeoverride), &key, &keylen, &intkey, 0, NULL)) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Key of MIME type overrides array must be a file extension, was \"%d\"", intkey);
				phar_entry_delref(phar TSRMLS_CC);
#ifdef PHP_WIN32
				efree(fname);
#endif
				RETURN_FALSE;
			}
			if (FAILURE == zend_hash_get_current_data(Z_ARRVAL_P(mimeoverride), (void **) &val)) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Failed to retrieve Mime type for extension \"%s\"", key);
				phar_entry_delref(phar TSRMLS_CC);
#ifdef PHP_WIN32
				efree(fname);
#endif
				RETURN_FALSE;
			}
			switch (Z_TYPE_PP(val)) {
				case IS_LONG :
					if (Z_LVAL_PP(val) == PHAR_MIME_PHP || Z_LVAL_PP(val) == PHAR_MIME_PHPS) {
						PHAR_SET_USER_MIME((char) Z_LVAL_PP(val))
					} else {
						zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unknown mime type specifier used, only Phar::PHP, Phar::PHPS and a mime type string are allowed");
						phar_entry_delref(phar TSRMLS_CC);
#ifdef PHP_WIN32
						efree(fname);
#endif
						RETURN_FALSE;
					}
					break;
				case IS_STRING :
					PHAR_SET_USER_MIME(PHAR_MIME_OTHER)
					break;
				default :
					zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unknown mime type specifier used (not a string or int), only Phar::PHP, Phar::PHPS and a mime type string are allowed");
					phar_entry_delref(phar TSRMLS_CC);
#ifdef PHP_WIN32
					efree(fname);
#endif
					RETURN_FALSE;
			}
		}
	}

no_mimes:
	code = phar_file_type(&mimetypes, entry, &mime_type TSRMLS_CC);
	zend_hash_destroy(&mimetypes);
	ret = phar_file_action(phar, mime_type, code, entry, entry_len, fname, fname_len, pt, strlen(pt), ru, ru_len TSRMLS_CC);
}
/* }}} */

/* {{{ proto void Phar::mungServer(array munglist)
 * Defines a list of up to 4 $_SERVER variables that should be modified for execution
 * to mask the presence of the phar archive.  This should be used in conjunction with
 * Phar::webPhar(), and has no effect otherwise
 * SCRIPT_NAME, PHP_SELF, REQUEST_URI and SCRIPT_FILENAME
 */
PHP_METHOD(Phar, mungServer)
{
	zval *mungvalues;
	int php_self = 0, request_uri = 0, script_name = 0, script_filename = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &mungvalues) == FAILURE) {
		return;
	}

	if (!zend_hash_num_elements(Z_ARRVAL_P(mungvalues))) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "No values passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
		return;
	}
	if (zend_hash_num_elements(Z_ARRVAL_P(mungvalues)) > 4) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Too many values passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
		return;
	}

	phar_request_initialize(TSRMLS_C);

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(mungvalues)); SUCCESS == zend_hash_has_more_elements(Z_ARRVAL_P(mungvalues)); zend_hash_move_forward(Z_ARRVAL_P(mungvalues))) {
		zval **data = NULL;

		if (SUCCESS != zend_hash_get_current_data(Z_ARRVAL_P(mungvalues), (void **) &data)) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "unable to retrieve array value in Phar::mungServer()");
			return;
		}
		if (Z_TYPE_PP(data) != IS_STRING) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Non-string value passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
			return;
		}
		if (!php_self && Z_STRLEN_PP(data) == sizeof("PHP_SELF")-1 && !strncmp(Z_STRVAL_PP(data), "PHP_SELF", sizeof("PHP_SELF")-1)) {
			if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "PHP_SELF", sizeof("PHP_SELF")-1)) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unable to add PHP_SELF to Phar::mungServer() list of values to mung");
				return;
			}
			php_self = 1;
		}
		if (Z_STRLEN_PP(data) == sizeof("REQUEST_URI")-1) {
			if (!request_uri && !strncmp(Z_STRVAL_PP(data), "REQUEST_URI", sizeof("REQUEST_URI")-1)) {
				if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "REQUEST_URI", sizeof("REQUEST_URI")-1)) {
					zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unable to add REQUEST_URI to Phar::mungServer() list of values to mung");
					return;
				}
				request_uri = 1;
			}
			if (!script_name && !strncmp(Z_STRVAL_PP(data), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
				if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
					zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unable to add SCRIPT_NAME to Phar::mungServer() list of values to mung");
					return;
				}
				script_name = 1;
			}
		}
		if (!script_filename && Z_STRLEN_PP(data) == sizeof("SCRIPT_FILENAME")-1 && !strncmp(Z_STRVAL_PP(data), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
			if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Unable to add SCRIPT_FILENAME to Phar::mungServer() list of values to mung");
				return;
			}
			script_filename = 1;
		}
	}
}
/* }}} */

/* {{{ proto void Phar::interceptFileFuncs()
 * instructs phar to intercept fopen, file_get_contents, opendir, and all of the stat-related functions
 * and return stat on files within the phar for relative paths
 *
 * Once called, this cannot be reversed, and continue until the end of the request.
 *
 * This allows legacy scripts to be pharred unmodified
 */
PHP_METHOD(Phar, interceptFileFuncs)
{
	phar_intercept_functions(TSRMLS_C);
}
/* }}} */

/* {{{ proto array Phar::createDefaultStub([string indexfile[, string webindexfile]])
 * Return a stub that can be used to run a phar-based archive without the phar extension
 * indexfile is the CLI startup filename, which defaults to "index.php", webindexfile
 * is the web startup filename, and also defaults to "index.php"
 */
PHP_METHOD(Phar, createDefaultStub)
{
	char *index = NULL, *webindex = NULL, *stub, *error;
	int index_len, webindex_len;
	size_t stub_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ss", &index, &index_len, &webindex, &webindex_len) == FAILURE) {
		return;
	}

	stub = phar_create_default_stub(index, webindex, &stub_len, &error TSRMLS_CC);

	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
		return;
	}
	RETURN_STRINGL(stub, stub_len, 0);
}
/* }}} */

/* {{{ proto mixed Phar::mapPhar([string alias, [int dataoffset]])
 * Reads the currently executed file (a phar) and registers its manifest */
PHP_METHOD(Phar, mapPhar)
{
	char *fname, *alias = NULL, *error, *plain_map;
	int fname_len, alias_len = 0;
	long dataoffset;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!l", &alias, &alias_len, &dataoffset) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	if (zend_hash_num_elements(&(PHAR_GLOBALS->phar_plain_map))) {
		fname = zend_get_executed_filename(TSRMLS_C);
		fname_len = strlen(fname);
		if((alias && 
		    zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), alias, alias_len+1, (void **)&plain_map) == SUCCESS)
		|| (zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), fname, fname_len+1, (void **)&plain_map) == SUCCESS)
		) {
			RETURN_STRING(plain_map, 1);
		}
	}

	RETVAL_BOOL(phar_open_compiled_file(alias, alias_len, &error TSRMLS_CC) == SUCCESS);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
} /* }}} */

/* {{{ proto mixed Phar::loadPhar(string filename [, string alias])
 * Loads any phar archive with an alias */
PHP_METHOD(Phar, loadPhar)
{
	char *fname, *alias = NULL, *error, *plain_map;
	int fname_len, alias_len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &fname, &fname_len, &alias, &alias_len) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	if (zend_hash_num_elements(&(PHAR_GLOBALS->phar_plain_map))) {
		if((alias && 
		    zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), alias, alias_len+1, (void **)&plain_map) == SUCCESS)
		|| (zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), fname, fname_len+1, (void **)&plain_map) == SUCCESS)
		) {
			RETURN_STRING(plain_map, 1);
		}
	}

	RETVAL_BOOL(phar_open_filename(fname, fname_len, alias, alias_len, REPORT_ERRORS, NULL, &error TSRMLS_CC) == SUCCESS);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
} /* }}} */

/* {{{ proto string Phar::apiVersion()
 * Returns the api version */
PHP_METHOD(Phar, apiVersion)
{
	RETURN_STRINGL(PHP_PHAR_API_VERSION, sizeof(PHP_PHAR_API_VERSION)-1, 1);
}
/* }}}*/

/* {{{ proto bool Phar::canCompress([int method])
 * Returns whether phar extension supports compression using zlib/bzip2 */
PHP_METHOD(Phar, canCompress)
{
	long method = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &method) == FAILURE) {
		return;
	}

	switch (method) {
	case PHAR_ENT_COMPRESSED_GZ:
		if (phar_has_zlib) {
			RETURN_TRUE;
		} else {
			RETURN_FALSE;
		}

	case PHAR_ENT_COMPRESSED_BZ2:
		if (phar_has_bz2) {
			RETURN_TRUE;
		} else {
			RETURN_FALSE;
		}

	default:
		if (phar_has_zlib || phar_has_bz2) {
			RETURN_TRUE;
		} else {
			RETURN_FALSE;
		}
	}
}
/* }}} */

/* {{{ proto bool Phar::canWrite()
 * Returns whether phar extension supports writing and creating phars */
PHP_METHOD(Phar, canWrite)
{
	RETURN_BOOL(!PHAR_G(readonly));
}
/* }}} */

/* {{{ proto bool Phar::isValidPharFilename(string filename[, bool executable = true])
 * Returns whether the given filename is a valid phar filename */
PHP_METHOD(Phar, isValidPharFilename)
{
	char *fname;
	const char *ext_str;
	int fname_len, ext_len;
	zend_bool executable = 1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|b", &fname, &fname_len, &executable) == FAILURE) {
		return;
	}

	fname_len = executable;
	RETVAL_BOOL(phar_detect_phar_fname_ext(fname, 1, &ext_str, &ext_len, fname_len, 2, 1 TSRMLS_CC) == SUCCESS);
}
/* }}} */

#if HAVE_SPL
/**
 * from spl_directory
 */
static void phar_spl_foreign_dtor(spl_filesystem_object *object TSRMLS_DC) /* {{{ */
{
	phar_archive_delref((phar_archive_data *) object->oth TSRMLS_CC);
	object->oth = NULL;
}
/* }}} */

/**
 * from spl_directory
 */
static void phar_spl_foreign_clone(spl_filesystem_object *src, spl_filesystem_object *dst TSRMLS_DC) /* {{{ */
{
	phar_archive_data *phar_data = (phar_archive_data *) dst->oth;

	++(phar_data->refcount);
}
/* }}} */

static spl_other_handler phar_spl_foreign_handler = {
       phar_spl_foreign_dtor,
       phar_spl_foreign_clone
};
#endif /* HAVE_SPL */

/* {{{ proto void Phar::__construct(string fname [, int flags [, string alias]])
 * Construct a Phar archive object
 * {{{ proto void PharData::__construct(string fname [, int flags [, string alias]])
 * Construct a PharData archive object
 */
PHP_METHOD(Phar, __construct)
{
#if !HAVE_SPL
	zend_throw_exception_ex(zend_exception_get_default(TSRMLS_C), 0 TSRMLS_CC, "Cannot instantiate Phar object without SPL extension");
#else
	char *fname, *alias = NULL, *error, *arch, *entry = NULL, *save_fname, *objname;
	int fname_len, alias_len = 0, arch_len, entry_len, is_data;
	long flags = 0;
	phar_archive_object *phar_obj;
	phar_archive_data   *phar_data;
	zval *zobj = getThis(), arg1, arg2;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|ls!", &fname, &fname_len, &flags, &alias, &alias_len) == FAILURE) {
		return;
	}

	phar_obj = (phar_archive_object*)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (phar_obj->arc.archive) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot call constructor twice");
		return;
	}

#if PHP_VERSION_ID >= 60000
	objname = phar_obj->std.ce->name.s;
#else
	objname = phar_obj->std.ce->name;
#endif
	if (!strncmp(objname, "PharData", 8)) {
		is_data = 1;
	} else {
		is_data = 0;
	}

	if (SUCCESS == phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len, !is_data, 2 TSRMLS_CC)) {
		/* use arch (the basename for the archive) for fname instead of fname */
		/* this allows support for RecursiveDirectoryIterator of subdirectories */
		save_fname = fname;
#ifdef PHP_WIN32
		phar_unixify_path_separators(arch, arch_len);
#endif
		fname = arch;
		fname_len = arch_len;
#ifdef PHP_WIN32
	} else {
		arch = estrndup(fname, fname_len);
		arch_len = fname_len;
		save_fname = fname;
		fname = arch;
		phar_unixify_path_separators(arch, arch_len);
#endif
	}

	if (phar_open_or_create_filename(fname, fname_len, alias, alias_len, is_data, REPORT_ERRORS, &phar_data, &error TSRMLS_CC) == FAILURE) {

		if (fname == arch) {
			efree(arch);
			fname = save_fname;
		}
		if (entry) {
			efree(entry);
		}
		if (error) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"%s", error);
			efree(error);
		}
		return;
	}

	if (fname == arch) {
		efree(arch);
		fname = save_fname;
	}

	is_data = phar_data->is_data;
	++(phar_data->refcount);
	phar_obj->arc.archive = phar_data;
	phar_obj->spl.oth_handler = &phar_spl_foreign_handler;

	if (entry) {
		fname_len = spprintf(&fname, 0, "phar://%s%s", phar_data->fname, entry);
		efree(entry);
	} else {
		fname_len = spprintf(&fname, 0, "phar://%s", phar_data->fname);
	}

	INIT_PZVAL(&arg1);
	ZVAL_STRINGL(&arg1, fname, fname_len, 0);

	if (ZEND_NUM_ARGS() > 1) {
		INIT_PZVAL(&arg2);
		ZVAL_LONG(&arg2, flags);
		zend_call_method_with_2_params(&zobj, Z_OBJCE_P(zobj), 
			&spl_ce_RecursiveDirectoryIterator->constructor, "__construct", NULL, &arg1, &arg2);
	} else {
		zend_call_method_with_1_params(&zobj, Z_OBJCE_P(zobj), 
			&spl_ce_RecursiveDirectoryIterator->constructor, "__construct", NULL, &arg1);
	}

	phar_obj->arc.archive->is_data = is_data;
	phar_obj->spl.info_class = phar_ce_entry;

	efree(fname);
#endif /* HAVE_SPL */
}
/* }}} */

/* {{{ proto array Phar::getSupportedSignatures()
 * Return array of supported signature types
 */
PHP_METHOD(Phar, getSupportedSignatures)
{
	array_init(return_value);

	add_next_index_stringl(return_value, "MD5", 3, 1);
	add_next_index_stringl(return_value, "SHA-1", 5, 1);
#if HAVE_HASH_EXT
	add_next_index_stringl(return_value, "SHA-256", 7, 1);
	add_next_index_stringl(return_value, "SHA-512", 7, 1);
#endif
}
/* }}} */

/* {{{ proto array Phar::getSupportedCompression()
 * Return array of supported comparession algorithms
 */
PHP_METHOD(Phar, getSupportedCompression)
{
	array_init(return_value);

	if (phar_has_zlib) {
		add_next_index_stringl(return_value, "GZ", 2, 1);
	}
	if (phar_has_bz2) {
		add_next_index_stringl(return_value, "BZIP2", 5, 1);
	}
}
/* }}} */

#if HAVE_SPL

#define PHAR_ARCHIVE_OBJECT() \
	phar_archive_object *phar_obj = (phar_archive_object*)zend_object_store_get_object(getThis() TSRMLS_CC); \
	if (!phar_obj->arc.archive) { \
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Cannot call method on an uninitialized Phar object"); \
		return; \
	}

static int phar_build(zend_object_iterator *iter, void *puser TSRMLS_DC) /* {{{ */
{
	zval **value;
	zend_uchar key_type;
	zend_bool is_splfileinfo = 0, close_fp = 1;
	ulong int_key;
	struct _t {
		phar_archive_object *p;
		zend_class_entry *c;
		char *b;
		uint l;
		zval *ret;
	} *p_obj = (struct _t*) puser;
	uint str_key_len, base_len = p_obj->l, fname_len;
	phar_entry_data *data;
	php_stream *fp;
	long contents_len;
	char *fname, *error, *str_key, *base = p_obj->b, *opened, *save = NULL, *temp = NULL;
	zend_class_entry *ce = p_obj->c;
	phar_archive_object *phar_obj = p_obj->p;
	char *str = "[stream]";

	iter->funcs->get_current_data(iter, &value TSRMLS_CC);
	if (EG(exception)) {
		return ZEND_HASH_APPLY_STOP;
	}
	if (!value) {
		/* failure in get_current_data */
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned no value", ce->name);
		return ZEND_HASH_APPLY_STOP;
	}
	switch (Z_TYPE_PP(value)) {
		case IS_STRING :
			break;
		case IS_RESOURCE :
			php_stream_from_zval_no_verify(fp, value);
			if (!fp) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Iterator %s returned an invalid stream handle", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			if (iter->funcs->get_current_key) {
				key_type = iter->funcs->get_current_key(iter, &str_key, &str_key_len, &int_key TSRMLS_CC);
				if (EG(exception)) {
					return ZEND_HASH_APPLY_STOP;
				}
				if (key_type == HASH_KEY_IS_LONG) {
					zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
					return ZEND_HASH_APPLY_STOP;
				}
				save = str_key;
				if (str_key[str_key_len - 1] == '\0') str_key_len--;
			} else {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			close_fp = 0;
			opened = (char *) estrndup(str, sizeof("[stream]") + 1);
			goto after_open_fp;
		case IS_OBJECT :
			if (instanceof_function(Z_OBJCE_PP(value), spl_ce_SplFileInfo TSRMLS_CC)) {
				char *test = NULL;
				zval dummy;
				spl_filesystem_object *intern = (spl_filesystem_object*)zend_object_store_get_object(*value TSRMLS_CC);

				if (!base_len) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Iterator %s returns an SplFileInfo object, so base directory must be specified", ce->name);
					return ZEND_HASH_APPLY_STOP;
				}
				switch (intern->type) {
					case SPL_FS_DIR:
#if PHP_VERSION_ID >= 60000
						test = spl_filesystem_object_get_path(intern, NULL, NULL TSRMLS_CC).s;
#elif PHP_VERSION_ID >= 50300
						test = spl_filesystem_object_get_path(intern, NULL TSRMLS_CC);
#else
						test = intern->path;
#endif
						fname_len = spprintf(&fname, 0, "%s%c%s", test, DEFAULT_SLASH, intern->u.dir.entry.d_name);
						php_stat(fname, fname_len, FS_IS_DIR, &dummy TSRMLS_CC);
						if (Z_BVAL(dummy)) {
							/* ignore directories */
							efree(fname);
							return ZEND_HASH_APPLY_KEEP;
						}
						test = expand_filepath(fname, NULL TSRMLS_CC);
						if (test) {
							efree(fname);
							fname = test;
							fname_len = strlen(fname);
						}
						save = fname;
						is_splfileinfo = 1;
						goto phar_spl_fileinfo;
					case SPL_FS_INFO:
					case SPL_FS_FILE:
						fname = expand_filepath(intern->file_name, NULL TSRMLS_CC);
						fname_len = strlen(fname);
						save = fname;
						is_splfileinfo = 1;
						goto phar_spl_fileinfo;
				}
			}
			/* fall-through */
		default :
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid value (must return a string)", ce->name);
			return ZEND_HASH_APPLY_STOP;
	}

	fname = Z_STRVAL_PP(value);
	fname_len = Z_STRLEN_PP(value);

phar_spl_fileinfo:
	if (base_len) {
		temp = expand_filepath(base, NULL TSRMLS_CC);
		base = temp;
		base_len = strlen(base);
		if (strstr(fname, base)) {
			str_key_len = fname_len - base_len;
			if (str_key_len <= 0) {
				if (save) {
					efree(save);
					efree(temp);
				}
				return ZEND_HASH_APPLY_KEEP;
			}
			str_key = fname + base_len;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that is not in the base directory \"%s\"", ce->name, fname, base);
			if (save) {
				efree(save);
				efree(temp);
			}
			return ZEND_HASH_APPLY_STOP;
		}
	} else {
		if (iter->funcs->get_current_key) {
			key_type = iter->funcs->get_current_key(iter, &str_key, &str_key_len, &int_key TSRMLS_CC);
			if (EG(exception)) {
				return ZEND_HASH_APPLY_STOP;
			}
			if (key_type == HASH_KEY_IS_LONG) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			save = str_key;
			if (str_key[str_key_len - 1] == '\0') str_key_len--;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
			return ZEND_HASH_APPLY_STOP;
		}
	}
#if PHP_MAJOR_VERSION < 6
	if (PG(safe_mode) && (!php_checkuid(fname, NULL, CHECKUID_ALLOW_ONLY_FILE))) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that safe mode prevents opening", ce->name, fname);
		if (save) {
			efree(save);
		}
		if (temp) {
			efree(temp);
		}
		return ZEND_HASH_APPLY_STOP;
	}
#endif

	if (php_check_open_basedir(fname TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that open_basedir prevents opening", ce->name, fname);
		if (save) {
			efree(save);
		}
		if (temp) {
			efree(temp);
		}
		return ZEND_HASH_APPLY_STOP;
	}

	/* try to open source file, then create internal phar file and copy contents */
	fp = php_stream_open_wrapper(fname, "rb", STREAM_MUST_SEEK|0, &opened);
	if (!fp) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a file that could not be opened \"%s\"", ce->name, fname);
		if (save) {
			efree(save);
		}
		if (temp) {
			efree(temp);
		}
		return ZEND_HASH_APPLY_STOP;
	}

after_open_fp:
	if (!(data = phar_get_or_create_entry_data(phar_obj->arc.archive->fname, phar_obj->arc.archive->fname_len, str_key, str_key_len, "w+b", 0, &error TSRMLS_CC))) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s cannot be created: %s", str_key, error);
		efree(error);
		if (save) {
			efree(save);
		}
		if (temp) {
			efree(temp);
		}
		if (close_fp) {
			php_stream_close(fp);
		}
		return ZEND_HASH_APPLY_STOP;
	} else {
		if (error) {
			efree(error);
		}
		contents_len = php_stream_copy_to_stream(fp, data->fp, PHP_STREAM_COPY_ALL);
	}
	if (close_fp) {
		php_stream_close(fp);
	}

	add_assoc_string(p_obj->ret, str_key, opened, 0);

	if (save) {
		efree(save);
	}
	if (temp) {
		efree(temp);
	}

	data->internal_file->compressed_filesize = data->internal_file->uncompressed_filesize = contents_len;
	phar_entry_delref(data TSRMLS_CC);
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{{ proto array Phar::buildFromDirectory(string directory[, string regex])
 * Construct a phar archive from an existing directory, recursively.
 * Optional second parameter is a regular expression for filtering directory contents.
 * 
 * Return value is an array mapping phar index to actual files added.
 */
PHP_METHOD(Phar, buildFromDirectory)
{
	char *dir, *regex, *error;
	int dir_len, regex_len;
	zend_bool apply_reg = 0;
	zval arg, arg2, *iter, *iteriter, *regexiter = NULL;
	struct {
		phar_archive_object *p;
		zend_class_entry *c;
		char *b;
		uint l;
		zval *ret;
	} pass;

	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write to archive - write operations restricted by INI setting");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s", &dir, &dir_len, &regex, &regex_len) == FAILURE) {
		RETURN_FALSE;
	}

	MAKE_STD_ZVAL(iter);

	if (SUCCESS != object_init_ex(iter, spl_ce_RecursiveDirectoryIterator)) {
		zval_dtor(iter);
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to instantiate directory iterator for %s", phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	INIT_PZVAL(&arg);
	ZVAL_STRINGL(&arg, dir, dir_len, 0);

	zend_call_method_with_1_params(&iter, spl_ce_RecursiveDirectoryIterator, 
			&spl_ce_RecursiveDirectoryIterator->constructor, "__construct", NULL, &arg);

	MAKE_STD_ZVAL(iteriter);

	if (SUCCESS != object_init_ex(iteriter, spl_ce_RecursiveIteratorIterator)) {
		zval_dtor(iteriter);
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to instantiate directory iterator for %s", phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	zend_call_method_with_1_params(&iteriter, spl_ce_RecursiveIteratorIterator, 
			&spl_ce_RecursiveIteratorIterator->constructor, "__construct", NULL, iter);

	zval_ptr_dtor(&iter);

	if (regex_len > 0) {
		apply_reg = 1;
		MAKE_STD_ZVAL(regexiter);

		if (SUCCESS != object_init_ex(regexiter, spl_ce_RegexIterator)) {
			zval_dtor(regexiter);
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to instantiate regex iterator for %s", phar_obj->arc.archive->fname);
			RETURN_FALSE;
		}

		INIT_PZVAL(&arg2);
		ZVAL_STRINGL(&arg2, regex, regex_len, 0);

		zend_call_method_with_2_params(&regexiter, spl_ce_RegexIterator, 
			&spl_ce_RegexIterator->constructor, "__construct", NULL, iteriter, &arg2);
	}

	array_init(return_value);

	pass.c = apply_reg ? Z_OBJCE_P(regexiter) : Z_OBJCE_P(iteriter);
	pass.p = phar_obj;
	pass.b = dir;
	pass.l = dir_len;
	pass.ret = return_value;

	if (SUCCESS == spl_iterator_apply((apply_reg ? regexiter : iteriter), (spl_iterator_apply_func_t) phar_build, (void *) &pass TSRMLS_CC)) {
		zval_ptr_dtor(&iteriter);
		if (apply_reg) {
			zval_ptr_dtor(&regexiter);
		}
		phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}
}

/* {{{ proto array Phar::buildFromIterator(Iterator iter[, string base_directory])
 * Construct a phar archive from an iterator.  The iterator must return a series of strings
 * that are full paths to files that should be added to the phar.  The iterator key should
 * be the path that the file will have within the phar archive.
 *
 * If base directory is specified, then the key will be ignored, and instead the portion of
 * the current value minus the base directory will be used
 *
 * Returned is an array mapping phar index to actual file added
 */
PHP_METHOD(Phar, buildFromIterator)
{
	zval *obj;
	char *error;
	uint base_len = 0;
	char *base;
	struct {
		phar_archive_object *p;
		zend_class_entry *c;
		char *b;
		uint l;
		zval *ret;
	} pass;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|s", &obj, zend_ce_traversable, &base, &base_len) == FAILURE) {
		RETURN_FALSE;
	}

	array_init(return_value);

	pass.c = Z_OBJCE_P(obj);
	pass.p = phar_obj;
	pass.b = base;
	pass.l = base_len;
	pass.ret = return_value;

	if (SUCCESS == spl_iterator_apply(obj, (spl_iterator_apply_func_t) phar_build, (void *) &pass TSRMLS_CC)) {
		phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}

}
/* }}} */

/* {{{ proto int Phar::count()
 * Returns the number of entries in the Phar archive
 */
PHP_METHOD(Phar, count)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_LONG(zend_hash_num_elements(&phar_obj->arc.archive->manifest));
}
/* }}} */

/* {{{ proto bool Phar::isTar()
 * Returns true if the phar archive is based on the tar file format
 */
PHP_METHOD(Phar, isTar)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_BOOL(phar_obj->arc.archive->is_tar);
}
/* }}} */

/* {{{ proto bool Phar::isZip()
 * Returns true if the phar archive is based on the Zip file format
 */
PHP_METHOD(Phar, isZip)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_BOOL(phar_obj->arc.archive->is_zip);
}
/* }}} */

/* {{{ proto bool Phar::isPhar()
 * Returns true if the phar archive is based on the phar file format
 */
PHP_METHOD(Phar, isPhar)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_BOOL(!phar_obj->arc.archive->is_tar && !phar_obj->arc.archive->is_zip);
}
/* }}} */

static int phar_copy_file_contents(phar_entry_info *entry, php_stream *fp TSRMLS_DC) /* {{{ */
{
	char *error;
	off_t offset;
	phar_entry_info *link;

	if (FAILURE == phar_open_entry_fp(entry, &error, 1 TSRMLS_CC)) {
		if (error) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot convert phar archive \"%s\", unable to open entry \"%s\" contents: %s", entry->phar->fname, entry->filename, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot convert phar archive \"%s\", unable to open entry \"%s\" contents", entry->phar->fname, entry->filename);
		}
		return FAILURE;
	}
	/* copy old contents in entirety */
	phar_seek_efp(entry, 0, SEEK_SET, 0, 1 TSRMLS_CC);
	offset = php_stream_tell(fp);
	link = phar_get_link_source(entry TSRMLS_CC);
	if (!link) {
		link = entry;
	}
	if (link->uncompressed_filesize != php_stream_copy_to_stream(phar_get_efp(link, 0 TSRMLS_CC), fp, link->uncompressed_filesize)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot convert phar archive \"%s\", unable to copy entry \"%s\" contents", entry->phar->fname, entry->filename);
		return FAILURE;
	}
	if (entry->fp_type == PHAR_MOD) {
		/* save for potential restore on error */
		entry->cfp = entry->fp;
		entry->fp = NULL;
	}
	/* set new location of file contents */
	entry->fp_type = PHAR_FP;
	entry->offset = offset;
	return SUCCESS;
}
/* }}} */

static zval *phar_rename_archive(phar_archive_data *phar, char *ext, zend_bool compress TSRMLS_DC)
{
	char *oldname = NULL, *oldpath = NULL;
	char *basename = NULL, *basepath = NULL;
	char *newname = NULL, *newpath = NULL;
	zval *ret, arg1;
	zend_class_entry *ce;
	char *error;

	if (!ext) {
		if (phar->is_zip) {
			if (phar->is_data) {
				ext = "zip";
			} else {
				ext = "phar.zip";
			}
		} else if (phar->is_tar) {
			switch (phar->flags) {
				case PHAR_FILE_COMPRESSED_GZ:
					if (phar->is_data) {
						ext = "tar.gz";
					} else {
						ext = "phar.tar.gz";
					}
					break;
				case PHAR_FILE_COMPRESSED_BZ2:
					if (phar->is_data) {
						ext = "tar.bz2";
					} else {
						ext = "phar.tar.bz2";
					}
					break;
				default:
					if (phar->is_data) {
						ext = "tar";
					} else {
						ext = "phar.tar";
					}
			}
		} else {
			switch (phar->flags) {
				case PHAR_FILE_COMPRESSED_GZ:
					ext = "phar.gz";
					break;
				case PHAR_FILE_COMPRESSED_BZ2:
					ext = "phar.bz2";
					break;
				default:
					ext = "phar";
			}
		}
	}

	if (ext[0] == '.') {
		++ext;
	}

	oldpath = estrndup(phar->fname, phar->fname_len);
	oldname = strrchr(phar->fname, '/');
	++oldname;

	basename = estrndup(oldname, strlen(oldname));
	spprintf(&newname, 0, "%s.%s", strtok(basename, "."), ext);
	efree(basename);

	basepath = estrndup(oldpath, strlen(oldpath) - strlen(oldname));
	phar->fname_len = spprintf(&newpath, 0, "%s%s", basepath, newname);
	phar->fname = newpath;
	efree(basepath);
	efree(newname);

	if (zend_hash_exists(&(PHAR_GLOBALS->phar_fname_map), newpath, phar->fname_len)) {
		efree(oldpath);
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to add newly converted phar \"%s\" to the list of phars, a phar with that name already exists", phar->fname);
		return NULL;
	}
	if (SUCCESS != zend_hash_add(&(PHAR_GLOBALS->phar_fname_map), newpath, phar->fname_len, (void*)&phar, sizeof(phar_archive_data*), NULL)) {
		efree(oldpath);
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to add newly converted phar \"%s\" to the list of phars", phar->fname);
		return NULL;
	}

	if (!phar->is_data) {
		if (phar->alias) {
			if (phar->is_temporary_alias) {
				phar->alias = NULL;
				phar->alias_len = 0;
			} else {
				phar->alias = estrndup(newpath, strlen(newpath));
				phar->alias_len = strlen(newpath);
				phar->is_temporary_alias = 1;
				zend_hash_update(&(PHAR_GLOBALS->phar_alias_map), newpath, phar->fname_len, (void*)&phar, sizeof(phar_archive_data*), NULL);
			}
		}
	} else {
		phar->alias = NULL;
		phar->alias_len = 0;
	}

	
	phar_flush(phar, 0, 0, 1, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
		efree(oldpath);
		return NULL;
	}

	efree(oldpath);

	if (phar->is_data) {
		ce = phar_ce_data;
	} else {
		ce = phar_ce_archive;
	}

	MAKE_STD_ZVAL(ret);
	if (SUCCESS != object_init_ex(ret, ce)) {
		zval_dtor(ret);
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Unable to instantiate phar object when converting archive \"%s\"", phar->fname);
		return NULL;
	}
	INIT_PZVAL(&arg1);
	ZVAL_STRINGL(&arg1, phar->fname, phar->fname_len, 0);

	zend_call_method_with_1_params(&ret, ce, &ce->constructor, "__construct", NULL, &arg1);
	
	return ret;
}

static zval *phar_convert_to_other(phar_archive_data *source, int convert, char *ext, php_uint32 flags TSRMLS_DC) /* {{{ */
{
	phar_archive_data *phar;
	phar_entry_info *entry, newentry;
	zval *ret;

	phar = (phar_archive_data *) ecalloc(1, sizeof(phar_archive_data));
	/* set whole-archive compression and type from parameter */
	phar->flags = flags;

	phar->is_data = source->is_data;
	switch (convert) {
		case PHAR_FORMAT_TAR :
			phar->is_tar = 1;
			break;
		case PHAR_FORMAT_ZIP :
			phar->is_zip = 1;
			break;
		default :
			phar->is_data = 0;
			break;
	}

	zend_hash_init(&(phar->manifest), sizeof(phar_entry_info),
		zend_get_hash_value, destroy_phar_manifest_entry, 0);

	phar->fp = php_stream_fopen_tmpfile();
	phar->fname = source->fname;
	phar->fname_len = source->fname_len;
	phar->is_temporary_alias = source->is_temporary_alias;
	phar->alias = source->alias;
	/* first copy each file's uncompressed contents to a temporary file and set per-file flags */
	for (zend_hash_internal_pointer_reset(&source->manifest); SUCCESS == zend_hash_has_more_elements(&source->manifest); zend_hash_move_forward(&source->manifest)) {

		if (FAILURE == zend_hash_get_current_data(&source->manifest, (void **) &entry)) {
			zend_hash_destroy(&(phar->manifest));
			php_stream_close(phar->fp);
			efree(phar);
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot convert phar archive \"%s\"", source->fname);
			return NULL;
		}
		newentry = *entry;
		if (FAILURE == phar_copy_file_contents(&newentry, phar->fp TSRMLS_CC)) {
			zend_hash_destroy(&(phar->manifest));
			php_stream_close(phar->fp);
			efree(phar);
			/* exception already thrown */
			return NULL;
		}
		newentry.filename = estrndup(newentry.filename, newentry.filename_len);
		if (newentry.metadata) {
			zval *t;

			t = newentry.metadata;
			ALLOC_ZVAL(newentry.metadata);
			*newentry.metadata = *t;
			zval_copy_ctor(newentry.metadata);
#if PHP_VERSION_ID < 50300
			newentry.metadata->refcount = 1;
#else
			Z_SET_REFCOUNT_P(newentry.metadata, 1);
#endif

			newentry.metadata_str.c = NULL;
			newentry.metadata_str.len = 0;
		}
		newentry.is_zip = phar->is_zip;
		newentry.is_tar = phar->is_tar;
		if (newentry.is_tar) {
			newentry.tar_type = (entry->is_dir ? TAR_DIR : TAR_FILE);
		}
		newentry.is_modified = 1;
		newentry.phar = phar;
 		newentry.old_flags = newentry.flags & ~PHAR_ENT_COMPRESSION_MASK; /* remove compression from old_flags */
		zend_hash_add(&(phar->manifest), newentry.filename, newentry.filename_len, (void*)&newentry, sizeof(phar_entry_info), NULL);
	}

	if ((ret = phar_rename_archive(phar, ext, 0 TSRMLS_CC))) {
		return ret;
	} else {
		zend_hash_destroy(&(phar->manifest));
		php_stream_close(phar->fp);
		efree(phar->fname);
		efree(phar);
		return NULL;
	}
}
/* }}} */

/* {{{ proto object Phar::convertToExecutable([int format[, int compression [, string file_ext]]])
 * Convert a phar.tar or phar.zip archive to the phar file format. The 
 * optional parameter allows the user to determine the new
 * filename extension (default is phar).
 */
PHP_METHOD(Phar, convertToExecutable)
{
	char *ext = NULL;
	int is_data, ext_len = 0;
	php_uint32 flags;
	zval *ret;
	/* a number that is not 0, 1 or 2 (Which is also Greg's birthday, so there) */
	long format = 9021976, method = 9021976;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lls", &format, &method, &ext, &ext_len) == FAILURE) {
		return;
	}

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out executable phar archive, phar is read-only");
		return;
	}

	switch (format) {
		case 9021976:
		case PHAR_FORMAT_SAME: /* null is converted to 0 */
			/* by default, use the existing format */
			if (phar_obj->arc.archive->is_tar) {
				format = PHAR_FORMAT_TAR;
			} else if (phar_obj->arc.archive->is_zip) {
				format = PHAR_FORMAT_ZIP;
			} else {
				format = PHAR_FORMAT_PHAR;
			}
			break;
		case PHAR_FORMAT_PHAR:
		case PHAR_FORMAT_TAR:
		case PHAR_FORMAT_ZIP:
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown file format specified, please pass one of Phar::PHAR, Phar::TAR or Phar::ZIP");
			return;
	}

	switch (method) {
		case 9021976:
			flags = phar_obj->arc.archive->flags & PHAR_FILE_COMPRESSION_MASK;
			break;
		case 0:
			flags = PHAR_FILE_COMPRESSED_NONE;
			break;
		case PHAR_ENT_COMPRESSED_GZ:
			if (format == PHAR_FORMAT_ZIP) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with gzip, zip archives do not support whole-archive compression");
				return;
			}
			if (!phar_has_zlib) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with gzip, enable ext/zlib in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_GZ;
			break;
	
		case PHAR_ENT_COMPRESSED_BZ2:
			if (format == PHAR_FORMAT_ZIP) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with bz2, zip archives do not support whole-archive compression");
				return;
			}
			if (!phar_has_bz2) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with bz2, enable ext/bz2 in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_BZ2;
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown compression specified, please pass one of Phar::GZ or Phar::BZ2");
			return;
	}

	is_data = phar_obj->arc.archive->is_data;
	phar_obj->arc.archive->is_data = 0;
	ret = phar_convert_to_other(phar_obj->arc.archive, format, ext, flags TSRMLS_CC);
	phar_obj->arc.archive->is_data = is_data;
	if (ret) {
		RETURN_ZVAL(ret, 1, 1);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto object Phar::convertToData([int format[, int compression [, string file_ext]]])
 * Convert an archive to a non-executable .tar or .zip.
 * The optional parameter allows the user to determine the new
 * filename extension (default is .zip or .tar).
 */
PHP_METHOD(Phar, convertToData)
{
	char *ext = NULL;
	int is_data, ext_len = 0;
	php_uint32 flags;
	zval *ret;
	/* a number that is not 0, 1 or 2 (Which is also Greg's birthday so there) */
	long format = 9021976, method = 9021976;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lls", &format, &method, &ext, &ext_len) == FAILURE) {
		return;
	}

	switch (format) {
		case 9021976:
		case PHAR_FORMAT_SAME: /* null is converted to 0 */
			/* by default, use the existing format */
			if (phar_obj->arc.archive->is_tar) {
				format = PHAR_FORMAT_TAR;
			} else if (phar_obj->arc.archive->is_zip) {
				format = PHAR_FORMAT_ZIP;
			} else {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
					"Cannot write out data phar archive, use Phar::TAR or Phar::ZIP");
				return;
			}
			break;
		case PHAR_FORMAT_PHAR:
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot write out data phar archive, use Phar::TAR or Phar::ZIP");
			return;
		case PHAR_FORMAT_TAR:
		case PHAR_FORMAT_ZIP:
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown file format specified, please pass one of Phar::TAR or Phar::ZIP");
			return;
	}

	switch (method) {
		case 9021976:
			flags = phar_obj->arc.archive->flags & PHAR_FILE_COMPRESSION_MASK;
			break;
		case 0:
			flags = PHAR_FILE_COMPRESSED_NONE;
			break;
		case PHAR_ENT_COMPRESSED_GZ:
			if (format == PHAR_FORMAT_ZIP) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with gzip, zip archives do not support whole-archive compression");
				return;
			}
			if (!phar_has_zlib) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with gzip, enable ext/zlib in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_GZ;
			break;
	
		case PHAR_ENT_COMPRESSED_BZ2:
			if (format == PHAR_FORMAT_ZIP) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with bz2, zip archives do not support whole-archive compression");
				return;
			}
			if (!phar_has_bz2) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with bz2, enable ext/bz2 in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_BZ2;
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown compression specified, please pass one of Phar::GZ or Phar::BZ2");
			return;
	}

	is_data = phar_obj->arc.archive->is_data;
	phar_obj->arc.archive->is_data = 1;
	ret = phar_convert_to_other(phar_obj->arc.archive, format, ext, flags TSRMLS_CC);
	phar_obj->arc.archive->is_data = is_data;
	if (ret) {
		RETURN_ZVAL(ret, 1, 1);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto int|false Phar::isCompressed()
 * Returns Phar::GZ or PHAR::BZ2 if the entire archive is compressed
 * (.tar.gz/tar.bz2 and so on), or FALSE otherwise.
 */
PHP_METHOD(Phar, isCompressed)
{
	PHAR_ARCHIVE_OBJECT();
	
	if (phar_obj->arc.archive->flags & PHAR_FILE_COMPRESSED_GZ) {
		RETURN_LONG(PHAR_ENT_COMPRESSED_GZ);
	}
	if (phar_obj->arc.archive->flags & PHAR_FILE_COMPRESSED_BZ2) {
		RETURN_LONG(PHAR_ENT_COMPRESSED_BZ2);
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool Phar::isWritable()
 * Returns true if phar.readonly=0 or phar is a PharData AND the actual file is writable.
 */
PHP_METHOD(Phar, isWritable)
{
	php_stream_statbuf ssb;
	PHAR_ARCHIVE_OBJECT();
	
	if (!phar_obj->arc.archive->is_writeable) {
		RETURN_FALSE;
	}
	if (SUCCESS != php_stream_stat_path(phar_obj->arc.archive->fname, &ssb)) {
		if (phar_obj->arc.archive->is_brandnew) {
			/* assume it works if the file doesn't exist yet */
			RETURN_TRUE;
		}
		RETURN_FALSE;
	}
	RETURN_BOOL((ssb.sb.st_mode & (S_IWOTH | S_IWGRP | S_IWUSR)) != 0);
}
/* }}} */

/* {{{ proto bool Phar::delete(string entry)
 * Deletes a named file within the archive.
 */
PHP_METHOD(Phar, delete)
{
	char *fname;
	int fname_len;
	char *error;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		RETURN_FALSE;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				RETURN_TRUE;
			} else {
				entry->is_deleted = 1;
				entry->is_modified = 1;
				phar_obj->arc.archive->is_modified = 1;
			}
		}
	} else {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be deleted", fname);
		RETURN_FALSE;
	}

	phar_flush(phar_obj->arc.archive, NULL, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
		
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int Phar::getAlias()
 * Returns the alias for the Phar or NULL.
 */
PHP_METHOD(Phar, getAlias)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->alias && phar_obj->arc.archive->alias != phar_obj->arc.archive->fname) {
		RETURN_STRINGL(phar_obj->arc.archive->alias, phar_obj->arc.archive->alias_len, 1);
	}
}
/* }}} */

/* {{{ proto int Phar::getPath()
 * Returns the real path to the phar archive on disk
 */
PHP_METHOD(Phar, getPath)
{
	PHAR_ARCHIVE_OBJECT();

	RETURN_STRINGL(phar_obj->arc.archive->fname, phar_obj->arc.archive->fname_len, 1);
}
/* }}} */

/* {{{ proto bool Phar::setAlias(string alias)
 * Sets the alias for a Phar archive. The default value is the full path
 * to the archive.
 */
PHP_METHOD(Phar, setAlias)
{
	char *alias, *error, *oldalias;
	phar_archive_data **fd_ptr;
	int alias_len, oldalias_len, old_temp, readd = 0;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
		RETURN_FALSE;
	}

	if (phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"A Phar alias cannot be set in a plain %s archive", phar_obj->arc.archive->is_tar ? "tar" : "zip");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &alias, &alias_len) == SUCCESS) {
		if (alias_len == phar_obj->arc.archive->alias_len && memcmp(phar_obj->arc.archive->alias, alias, alias_len) == 0) {
			RETURN_TRUE;
		}
		if (alias_len && SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_alias_map), alias, alias_len, (void**)&fd_ptr)) {
			spprintf(&error, 0, "alias \"%s\" is already used for archive \"%s\" and cannot be used for other archives", alias, (*fd_ptr)->fname);
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		}
		if (!phar_validate_alias(alias, alias_len)) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Invalid alias \"%s\" specified for phar \"%s\"", alias, phar_obj->arc.archive->fname);
			RETURN_FALSE;
		}
		if (phar_obj->arc.archive->alias_len && SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_alias_map), phar_obj->arc.archive->alias, phar_obj->arc.archive->alias_len, (void**)&fd_ptr)) {
			zend_hash_del(&(PHAR_GLOBALS->phar_alias_map), phar_obj->arc.archive->alias, phar_obj->arc.archive->alias_len);
			readd = 1;
		}

		oldalias = phar_obj->arc.archive->alias;
		oldalias_len = phar_obj->arc.archive->alias_len;
		old_temp = phar_obj->arc.archive->is_temporary_alias;
		if (alias_len) {
			phar_obj->arc.archive->alias = estrndup(alias, alias_len);
		} else {
			phar_obj->arc.archive->alias = NULL;
		}
		phar_obj->arc.archive->alias_len = alias_len;
		phar_obj->arc.archive->is_temporary_alias = 0;

		phar_flush(phar_obj->arc.archive, NULL, 0, 0, &error TSRMLS_CC);
		if (error) {
			phar_obj->arc.archive->alias = oldalias;
			phar_obj->arc.archive->alias_len = oldalias_len;
			phar_obj->arc.archive->is_temporary_alias = old_temp;
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			if (readd) {
				zend_hash_add(&(PHAR_GLOBALS->phar_alias_map), oldalias, oldalias_len, (void*)&(phar_obj->arc.archive), sizeof(phar_archive_data*), NULL);
			}
			efree(error);
			RETURN_FALSE;
		}
		zend_hash_add(&(PHAR_GLOBALS->phar_alias_map), alias, alias_len, (void*)&(phar_obj->arc.archive), sizeof(phar_archive_data*), NULL);
		if (oldalias) {
			efree(oldalias);
		}
		RETURN_TRUE;
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ proto string Phar::getVersion()
 * Return version info of Phar archive
 */
PHP_METHOD(Phar, getVersion)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_STRING(phar_obj->arc.archive->version, 1);
}
/* }}} */

/* {{{ proto void Phar::startBuffering()
 * Do not flush a writeable phar (save its contents) until explicitly requested
 */
PHP_METHOD(Phar, startBuffering)
{
	PHAR_ARCHIVE_OBJECT();
	
	phar_obj->arc.archive->donotflush = 1;
}
/* }}} */

/* {{{ proto bool Phar::isBuffering()
 * Returns whether write operations are flushing to disk immediately.
 */
PHP_METHOD(Phar, isBuffering)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_BOOL(!phar_obj->arc.archive->donotflush);
}
/* }}} */

/* {{{ proto bool Phar::stopBuffering()
 * Saves the contents of a modified archive to disk.
 */
PHP_METHOD(Phar, stopBuffering)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
		return;
	}

	phar_obj->arc.archive->donotflush = 0;

	phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool Phar::setStub(string|stream stub [, int len])
 * Change the stub in a phar, phar.tar or phar.zip archive to something other
 * than the default. The stub *must* end with a call to __HALT_COMPILER().
 */
PHP_METHOD(Phar, setStub)
{
	zval *zstub;
	char *stub, *error;
	int stub_len;
	long len = -1;
	php_stream *stream;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot change stub, phar is read-only");
		return;
	}

	if (phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"A Phar stub cannot be set in a plain %s archive", phar_obj->arc.archive->is_tar ? "tar" : "zip");
		return;
	}

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zstub, &len) == SUCCESS) {
		if ((php_stream_from_zval_no_verify(stream, &zstub)) != NULL) {
			if (len > 0) {
				len = -len;
			} else {
				len = -1;
			}
			phar_flush(phar_obj->arc.archive, (char *) &zstub, len, 0, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
				efree(error);
			}
			RETURN_TRUE;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot change stub, unable to read from input stream");
		}
	} else if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &stub, &stub_len) == SUCCESS) {
		phar_flush(phar_obj->arc.archive, stub, stub_len, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
		RETURN_TRUE;
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool Phar::setDefaultStub([string index[, string webindex]])
 * In a pure phar archive, sets a stub that can be used to run the archive
 * regardless of whether the phar extension is available. The first parameter
 * is the CLI startup filename, which defaults to "index.php". The second
 * parameter is the web startup filename and also defaults to "index.php"
 * (falling back to CLI behaviour).
 * Both parameters are optional.
 * In a phar.zip or phar.tar archive, the default stub is used only to
 * identify the archive to the extension as a Phar object. This allows the
 * extension to treat phar.zip and phar.tar types as honorary phars. Since
 * files cannot be loaded via this kind of stub, no parameters are accepted
 * when the Phar object is zip- or tar-based.
 */
 PHP_METHOD(Phar, setDefaultStub)
{
	char *index = NULL, *webindex = NULL, *error = NULL, *stub = NULL;
	int index_len = 0, webindex_len = 0, created_stub = 0;
	size_t stub_len = 0;
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"A Phar stub cannot be set in a plain %s archive", phar_obj->arc.archive->is_tar ? "tar" : "zip");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!s", &index, &index_len, &webindex, &webindex_len) == FAILURE) {
		RETURN_FALSE;
	}

	if (ZEND_NUM_ARGS() > 0 && (phar_obj->arc.archive->is_tar || phar_obj->arc.archive->is_zip)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "method accepts no arguments for a tar- or zip-based phar stub, %d given", ZEND_NUM_ARGS());
		RETURN_FALSE;
	}

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot change stub: phar.readonly=1");
		RETURN_FALSE;
	}

	if (!phar_obj->arc.archive->is_tar && !phar_obj->arc.archive->is_zip) {

		stub = phar_create_default_stub(index, webindex, &stub_len, &error TSRMLS_CC);

		if (error) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, error);
			efree(error);
			if (stub) {
				efree(stub);
			}
			RETURN_FALSE;
		}
		created_stub = 1;
	}

	phar_flush(phar_obj->arc.archive, stub, stub_len, 1, &error TSRMLS_CC);

	if (created_stub) {
		efree(stub);
	}

	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array Phar::setSignatureAlgorithm(int sigtype)
 * Sets the signature algorithm for a phar and applies it. The signature
 * algorithm must be one of Phar::MD5, Phar::SHA1, Phar::SHA256,
 * Phar::SHA512, or Phar::PGP (PGP is not yet supported and falls back to
 * SHA-1). Note that zip- and tar- based phar archives cannot support
 * signatures.
 */
PHP_METHOD(Phar, setSignatureAlgorithm)
{
	long algo;
	char *error;
	PHAR_ARCHIVE_OBJECT();
	
	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot set signature algorithm, phar is read-only");
		return;
	}
	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set signature algorithm, not possible with tar-based phar archives");
		return;
	}
	if (phar_obj->arc.archive->is_zip) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set signature algorithm, not possible with zip-based phar archives");
		return;
	}

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "l", &algo) != SUCCESS) {
		return;
	}

	switch (algo) {
		case PHAR_SIG_SHA256 :
		case PHAR_SIG_SHA512 :
#if !HAVE_HASH_EXT
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"SHA-256 and SHA-512 signatures are only supported if the hash extension is enabled");
			return;
#endif
		case PHAR_SIG_MD5 :
		case PHAR_SIG_SHA1 :
		case PHAR_SIG_PGP :
			phar_obj->arc.archive->sig_flags = algo;
			phar_obj->arc.archive->is_modified = 1;

			phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
				efree(error);
			}
			break;
		default :
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Unknown signature algorithm specified");
	}
}
/* }}} */

/* {{{ proto array|false Phar::getSignature()
 * Returns a hash signature, or FALSE if the archive is unsigned.
 */
PHP_METHOD(Phar, getSignature)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->signature) {
		array_init(return_value);
		add_assoc_stringl(return_value, "hash", phar_obj->arc.archive->signature, phar_obj->arc.archive->sig_len, 1);
		switch(phar_obj->arc.archive->sig_flags) {
		case PHAR_SIG_MD5:
			add_assoc_stringl(return_value, "hash_type", "MD5", 3, 1);
			break;
		case PHAR_SIG_SHA1:
			add_assoc_stringl(return_value, "hash_type", "SHA-1", 5, 1);
			break;
		case PHAR_SIG_SHA256:
			add_assoc_stringl(return_value, "hash_type", "SHA-256", 7, 1);
			break;
		case PHAR_SIG_SHA512:
			add_assoc_stringl(return_value, "hash_type", "SHA-512", 7, 1);
			break;
		}
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto bool Phar::getModified()
 * Return whether phar was modified
 */
PHP_METHOD(Phar, getModified)
{
	PHAR_ARCHIVE_OBJECT();

	RETURN_BOOL(phar_obj->arc.archive->is_modified);
}
/* }}} */

static int phar_set_compression(void *pDest, void *argument TSRMLS_DC) /* {{{ */
{
	phar_entry_info *entry = (phar_entry_info *)pDest;
	php_uint32 compress = *(php_uint32 *)argument;

	if (entry->is_deleted) {
		return ZEND_HASH_APPLY_KEEP;
	}
	entry->old_flags = entry->flags;
	entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry->flags |= compress;
	entry->is_modified = 1;
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static int phar_test_compression(void *pDest, void *argument TSRMLS_DC) /* {{{ */
{
	phar_entry_info *entry = (phar_entry_info *)pDest;

	if (entry->is_deleted) {
		return ZEND_HASH_APPLY_KEEP;
	}
	if (!phar_has_bz2) {
		if (entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
			*(int *) argument = 0;
		}
	}
	if (!phar_has_zlib) {
		if (entry->flags & PHAR_ENT_COMPRESSED_GZ) {
			*(int *) argument = 0;
		}
	}
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static void pharobj_set_compression(HashTable *manifest, php_uint32 compress TSRMLS_DC) /* {{{ */
{
	zend_hash_apply_with_argument(manifest, phar_set_compression, &compress TSRMLS_CC);
}
/* }}} */

static int pharobj_cancompress(HashTable *manifest TSRMLS_DC) /* {{{ */
{
	int test;
	test = 1;
	zend_hash_apply_with_argument(manifest, phar_test_compression, &test TSRMLS_CC);
	return test;
}
/* }}} */

/* {{{ proto object Phar::compress(int method[, string extension])
 * Compress a .tar, or .phar.tar with whole-file compression
 * The parameter can be one of Phar::GZ or Phar::BZ2 to specify
 * the kind of compression desired
 */
PHP_METHOD(Phar, compress)
{
	long method = 0;
	char *ext = NULL;
	int ext_len;
	php_uint32 flags;
	zval *ret;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|s", &method, &ext, &ext_len) == FAILURE) {
		return;
	}
	
	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot compress phar archive, phar is read-only");
		return;
	}

	if (phar_obj->arc.archive->is_zip) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot compress zip-based archives with whole-archive compression");
		return;
	}

	switch (method) {
		case 0:
			flags = PHAR_FILE_COMPRESSED_NONE;
			break;
		case PHAR_ENT_COMPRESSED_GZ:
			if (!phar_has_zlib) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with gzip, enable ext/zlib in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_GZ;
			break;
	
		case PHAR_ENT_COMPRESSED_BZ2:
			if (!phar_has_bz2) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress entire archive with bz2, enable ext/bz2 in php.ini");
				return;
			}
			flags = PHAR_FILE_COMPRESSED_BZ2;
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown compression specified, please pass one of Phar::GZ or Phar::BZ2");
			return;
	}

	if (phar_obj->arc.archive->is_tar) {
		ret = phar_convert_to_other(phar_obj->arc.archive, PHAR_FORMAT_TAR, ext, flags TSRMLS_CC);
	} else {
		ret = phar_convert_to_other(phar_obj->arc.archive, PHAR_FORMAT_PHAR, ext, flags TSRMLS_CC);
	}
	if (ret) {
		RETURN_ZVAL(ret, 1, 1);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto object Phar::decompress([string extension])
 * Decompress a .tar, or .phar.tar with whole-file compression
 */
PHP_METHOD(Phar, decompress)
{
	char *ext = NULL;
	int ext_len;
	zval *ret;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &ext, &ext_len) == FAILURE) {
		return;
	}

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot decompress phar archive, phar is read-only");
		return;
	}

	if (phar_obj->arc.archive->is_zip) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot decompress zip-based archives with whole-archive compression");
		return;
	}

	if (phar_obj->arc.archive->is_tar) {
		ret = phar_convert_to_other(phar_obj->arc.archive, PHAR_FORMAT_TAR, ext, PHAR_FILE_COMPRESSED_NONE TSRMLS_CC);
	} else {
		ret = phar_convert_to_other(phar_obj->arc.archive, PHAR_FORMAT_PHAR, ext, PHAR_FILE_COMPRESSED_NONE TSRMLS_CC);
	}
	if (ret) {
		RETURN_ZVAL(ret, 1, 1);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto object Phar::compressFiles(int method)
 * Compress all files within a phar or zip archive using the specified compression
 * The parameter can be one of Phar::GZ or Phar::BZ2 to specify
 * the kind of compression desired
 */
PHP_METHOD(Phar, compressFiles)
{
	char *error;
	php_uint32 flags;
	long method;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &method) == FAILURE) {
		return;
	}

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
		return;
	}

	switch (method) {
		case PHAR_ENT_COMPRESSED_GZ:
			if (!phar_has_zlib) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress files within archive with gzip, enable ext/zlib in php.ini");
				return;
			}
			flags = PHAR_ENT_COMPRESSED_GZ;
			break;

		case PHAR_ENT_COMPRESSED_BZ2:
			if (!phar_has_bz2) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress files within archive with bz2, enable ext/bz2 in php.ini");
				return;
			}
			flags = PHAR_ENT_COMPRESSED_BZ2;
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Unknown compression specified, please pass one of Phar::GZ or Phar::BZ2");
			return;
	}
	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Gzip compression, tar archives cannot compress individual files, use compress() to compress the whole archive");
		return;
	}
	if (!pharobj_cancompress(&phar_obj->arc.archive->manifest TSRMLS_CC)) {
		if (flags == PHAR_FILE_COMPRESSED_GZ) {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Cannot compress all files as Gzip, some are compressed as bzip2 and cannot be decompressed");
		} else {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
				"Cannot compress all files as Bzip2, some are compressed as gzip and cannot be decompressed");
		}
		return;
	}
	pharobj_set_compression(&phar_obj->arc.archive->manifest, flags TSRMLS_CC);

	phar_obj->arc.archive->is_modified = 1;

	phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool Phar::decompressFiles()
 * decompress every file
 */
PHP_METHOD(Phar, decompressFiles)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
		return;
	}
	if (!pharobj_cancompress(&phar_obj->arc.archive->manifest TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot decompress all files, some are compressed as bzip2 or gzip and cannot be decompressed");
		return;
	}
	if (phar_obj->arc.archive->is_tar) {
		RETURN_TRUE;
	} else {
		pharobj_set_compression(&phar_obj->arc.archive->manifest, PHAR_ENT_COMPRESSED_NONE TSRMLS_CC);
	}

	phar_obj->arc.archive->is_modified = 1;

	phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Phar::copy(string oldfile, string newfile)
 * copy a file internal to the phar archive to another new file within the phar
 */
PHP_METHOD(Phar, copy)
{
	char *oldfile, *newfile, *error;
	const char *pcr_error;
	int oldfile_len, newfile_len;
	phar_entry_info *oldentry, newentry = {0}, *temp;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &oldfile, &oldfile_len, &newfile, &newfile_len) == FAILURE) {
		return;
	}

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot copy \"%s\" to \"%s\", phar is read-only", oldfile, newfile);
		RETURN_FALSE;
	}

	if (!zend_hash_exists(&phar_obj->arc.archive->manifest, oldfile, (uint) oldfile_len) || SUCCESS != zend_hash_find(&phar_obj->arc.archive->manifest, oldfile, (uint) oldfile_len, (void**)&oldentry) || oldentry->is_deleted) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"file \"%s\" cannot be copied to file \"%s\", file does not exist in %s", oldfile, newfile, phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, newfile, (uint) newfile_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, newfile, (uint) newfile_len, (void**)&temp) || !temp->is_deleted) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"file \"%s\" cannot be copied to file \"%s\", file must not already exist in phar %s", oldfile, newfile, phar_obj->arc.archive->fname);
			RETURN_FALSE;
		}
	}

	if (phar_path_check(&newfile, &newfile_len, &pcr_error) > pcr_is_ok) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"file \"%s\" contains invalid characters %s, cannot be copied from \"%s\" in phar %s", newfile, pcr_error, oldfile, phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	memcpy((void *) &newentry, oldentry, sizeof(phar_entry_info));
	if (newentry.metadata) {
		zval *t;

		t = newentry.metadata;
		ALLOC_ZVAL(newentry.metadata);
		*newentry.metadata = *t;
		zval_copy_ctor(newentry.metadata);
#if PHP_VERSION_ID < 50300
		newentry.metadata->refcount = 1;
#else
		Z_SET_REFCOUNT_P(newentry.metadata, 1);
#endif

		newentry.metadata_str.c = NULL;
		newentry.metadata_str.len = 0;
	}
	newentry.filename = estrndup(newfile, newfile_len);
	newentry.filename_len = newfile_len;
	newentry.fp_refcount = 0;

	if (oldentry->fp_type != PHAR_FP) {
		if (FAILURE == phar_copy_entry_fp(oldentry, &newentry, &error TSRMLS_CC)) {
			efree(newentry.filename);
			php_stream_close(newentry.fp);
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			return;
		}
	}

	zend_hash_add(&oldentry->phar->manifest, newfile, newfile_len, (void*)&newentry, sizeof(phar_entry_info), NULL);
	phar_obj->arc.archive->is_modified = 1;

	phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int Phar::offsetExists(string entry)
 * determines whether a file exists in the phar
 */
PHP_METHOD(Phar, offsetExists)
{
	char *fname;
	int fname_len;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				RETURN_FALSE;
			}
		}
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto int Phar::offsetGet(string entry)
 * get a PharFileInfo object for a specific file
 */
PHP_METHOD(Phar, offsetGet)
{
	char *fname, *error;
	int fname_len;
	zval *zfname;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}
	
	if (!(entry = phar_get_entry_info_dir(phar_obj->arc.archive, fname, fname_len, 1, &error TSRMLS_CC))) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist%s%s", fname, error?", ":"", error?error:"");
	} else {
		if (entry->is_temp_dir) {
			efree(entry->filename);
			efree(entry);
		}
		fname_len = spprintf(&fname, 0, "phar://%s/%s", phar_obj->arc.archive->fname, fname);
		MAKE_STD_ZVAL(zfname);
		ZVAL_STRINGL(zfname, fname, fname_len, 0);
		spl_instantiate_arg_ex1(phar_obj->spl.info_class, &return_value, 0, zfname TSRMLS_CC);
		zval_ptr_dtor(&zfname);
	}

}
/* }}} */

/* {{{ add a file within the phar archive from a string or resource
 */
static void phar_add_file(phar_archive_data *phar, char *filename, int filename_len, char *cont_str, int cont_len, zval *zresource TSRMLS_DC)
{
	char *error;
	long contents_len;
	phar_entry_data *data;
	php_stream *contents_file;

	if (!(data = phar_get_or_create_entry_data(phar->fname, phar->fname_len, filename, filename_len, "w+b", 0, &error TSRMLS_CC))) {
		if (error) {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be created: %s", filename, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be created", filename);
		}
		return;
	} else {
		if (error) {
			efree(error);
		}
		if (!data->internal_file->is_dir) {
			if (cont_str) {
				contents_len = php_stream_write(data->fp, cont_str, cont_len);
				if (contents_len != cont_len) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s could not be written to", filename);
					return;
				}
			} else {
				if (!(php_stream_from_zval_no_verify(contents_file, &zresource))) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s could not be written to", filename);
					return;
				}
				contents_len = php_stream_copy_to_stream(contents_file, data->fp, PHP_STREAM_COPY_ALL);
			}
			data->internal_file->compressed_filesize = data->internal_file->uncompressed_filesize = contents_len;
		}
		phar_entry_delref(data TSRMLS_CC);
		phar_flush(phar, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}
}
/* }}} */

/* {{{ create a directory within the phar archive
 */
static void phar_mkdir(phar_archive_data *phar, char *dirname, int dirname_len TSRMLS_DC)
{
	char *error;
	phar_entry_data *data;

	if (!(data = phar_get_or_create_entry_data(phar->fname, phar->fname_len, dirname, dirname_len, "w+b", 2, &error TSRMLS_CC))) {
		if (error) {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Directory %s does not exist and cannot be created: %s", dirname, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Directory %s does not exist and cannot be created", dirname);
		}
		return;
	} else {
		if (error) {
			efree(error);
		}
		phar_entry_delref(data TSRMLS_CC);
		phar_flush(phar, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}
}
/* }}} */


/* {{{ proto int Phar::offsetSet(string entry, string value)
 * set the contents of an internal file to those of an external file
 */
PHP_METHOD(Phar, offsetSet)
{
	char *fname, *cont_str = NULL;
	int fname_len, cont_len;
	zval *zresource;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}
	
	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "sr", &fname, &fname_len, &zresource) == FAILURE
	&& zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &fname, &fname_len, &cont_str, &cont_len) == FAILURE) {
		return;
	}

	if ((phar_obj->arc.archive->is_tar || phar_obj->arc.archive->is_zip) && fname_len == sizeof(".phar/stub.php")-1 && !memcmp(fname, ".phar/stub.php", sizeof(".phar/stub.php")-1)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot set stub \".phar/stub.php\" directly in phar \"%s\", use setStub", phar_obj->arc.archive->fname);
		return;
	}

	if ((phar_obj->arc.archive->is_tar || phar_obj->arc.archive->is_zip) && fname_len == sizeof(".phar/alias.txt")-1 && !memcmp(fname, ".phar/alias.txt", sizeof(".phar/alias.txt")-1)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot set alias \".phar/alias.txt\" directly in phar \"%s\", use setAlias", phar_obj->arc.archive->fname);
		return;
	}
	phar_add_file(phar_obj->arc.archive, fname, fname_len, cont_str, cont_len, zresource TSRMLS_CC);
}
/* }}} */

/* {{{ proto int Phar::offsetUnset(string entry)
 * remove a file from a phar
 */
PHP_METHOD(Phar, offsetUnset)
{
	char *fname, *error;
	int fname_len;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				return;
			}
			entry->is_modified = 0;
			entry->is_deleted = 1;
			/* we need to "flush" the stream to save the newly deleted file on disk */
			phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
				efree(error);
			}
			RETURN_TRUE;
		}
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto string Phar::addEmptyDir(string dirname)
 * Adds an empty directory to the phar archive
 */
PHP_METHOD(Phar, addEmptyDir)
{
	char *dirname;
	int dirname_len;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &dirname, &dirname_len) == FAILURE) {
		return;
	}

	phar_mkdir(phar_obj->arc.archive, dirname, dirname_len TSRMLS_CC);
}
/* }}} */

/* {{{ proto string Phar::addFile(string filename[, string localname])
 * Adds a file to the archive using the filename, or the second parameter as the name within the archive
 */
PHP_METHOD(Phar, addFile)
{
	char *fname, *localname = NULL;
	int fname_len, localname_len;
	php_stream *resource;
	zval *zresource;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s", &fname, &fname_len, &localname, &localname_len) == FAILURE) {
		return;
	}

	if (!(resource = php_stream_open_wrapper(fname, "rb", 0, NULL))) {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC, "phar error: unable to open file \"%s\" to add to phar archive", fname);
		return;
	}
	if (localname) {
		fname = localname;
		fname_len = localname_len;
	}

	MAKE_STD_ZVAL(zresource);
	php_stream_to_zval(resource, zresource);
	phar_add_file(phar_obj->arc.archive, fname, fname_len, NULL, 0, zresource TSRMLS_CC);
	efree(zresource);
	php_stream_close(resource);
}
/* }}} */

/* {{{ proto string Phar::addFromString(string localname, string contents)
 * Adds a file to the archive using its contents as a string
 */
PHP_METHOD(Phar, addFromString)
{
	char *localname, *cont_str;
	int localname_len, cont_len;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &localname, &localname_len, &cont_str, &cont_len) == FAILURE) {
		return;
	}

	phar_add_file(phar_obj->arc.archive, localname, localname_len, cont_str, cont_len, NULL TSRMLS_CC);
}
/* }}} */

/* {{{ proto string Phar::getStub()
 * Returns the stub at the head of a phar archive as a string.
 */
PHP_METHOD(Phar, getStub)
{
	size_t len;
	char *buf;
	php_stream *fp;
	php_stream_filter *filter = NULL;
	phar_entry_info *stub;
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->is_tar || phar_obj->arc.archive->is_zip) {

		if (SUCCESS == zend_hash_find(&(phar_obj->arc.archive->manifest), ".phar/stub.php", sizeof(".phar/stub.php")-1, (void **)&stub)) {
			if (phar_obj->arc.archive->fp && !phar_obj->arc.archive->is_brandnew && !(stub->flags & PHAR_ENT_COMPRESSION_MASK)) {
				fp = phar_obj->arc.archive->fp;
			} else {
				fp = php_stream_open_wrapper(phar_obj->arc.archive->fname, "rb", 0, NULL);
				if (stub->flags & PHAR_ENT_COMPRESSION_MASK) {
					char *filter_name;

					if ((filter_name = phar_decompress_filter(stub, 0)) != NULL) {
						filter = php_stream_filter_create(phar_decompress_filter(stub, 0), NULL, php_stream_is_persistent(fp) TSRMLS_CC);
					} else {
						filter = NULL;
					}
					if (!filter) {
						zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "phar error: unable to read stub of phar \"%s\" (cannot create %s filter)", phar_obj->arc.archive->fname, phar_decompress_filter(stub, 1));
						return;
					}
					php_stream_filter_append(&fp->readfilters, filter);
				}
			}

			if (!fp)  {
				zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
					"Unable to read stub");
				return;
			}

			php_stream_seek(fp, stub->offset_abs, SEEK_SET);
			len = stub->uncompressed_filesize;
			goto carry_on;
		} else {
			RETURN_STRINGL("", 0, 1);
		}
	}
	len = phar_obj->arc.archive->halt_offset;

	if (phar_obj->arc.archive->fp && !phar_obj->arc.archive->is_brandnew) {
		fp = phar_obj->arc.archive->fp;
	} else {
		fp = php_stream_open_wrapper(phar_obj->arc.archive->fname, "rb", 0, NULL);
	}

	if (!fp)  {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"Unable to read stub");
		return;
	}

	php_stream_rewind(fp);
carry_on:
	buf = safe_emalloc(len, 1, 1);
	if (len != php_stream_read(fp, buf, len)) {
		if (fp != phar_obj->arc.archive->fp) {
			php_stream_close(fp);
		}
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"Unable to read stub");
		efree(buf);
		return;
	}
	if (filter) {
		php_stream_filter_flush(filter, 1);
		php_stream_filter_remove(filter, 1 TSRMLS_CC);
	}
	if (fp != phar_obj->arc.archive->fp) {
		php_stream_close(fp);
	}
	buf[len] = '\0';

	RETURN_STRINGL(buf, len, 0);
}
/* }}}*/

/* {{{ proto int Phar::hasMetaData()
 * Returns TRUE if the phar has global metadata, FALSE otherwise.
 */
PHP_METHOD(Phar, hasMetadata)
{
	PHAR_ARCHIVE_OBJECT();

	RETURN_BOOL(phar_obj->arc.archive->metadata != NULL);
}
/* }}} */

/* {{{ proto int Phar::getMetaData()
 * Returns the global metadata of the phar
 */
PHP_METHOD(Phar, getMetadata)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->metadata) {
		RETURN_ZVAL(phar_obj->arc.archive->metadata, 1, 0);
	}
}
/* }}} */

/* {{{ proto int Phar::setMetaData(mixed $metadata)
 * Sets the global metadata of the phar
 */
PHP_METHOD(Phar, setMetadata)
{
	char *error;
	zval *metadata;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}
	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set metadata, not possible with tar-based phar archives");
		return;
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &metadata) == FAILURE) {
		return;
	}

	if (phar_obj->arc.archive->metadata) {
		zval_ptr_dtor(&phar_obj->arc.archive->metadata);
		phar_obj->arc.archive->metadata = NULL;
	}

	MAKE_STD_ZVAL(phar_obj->arc.archive->metadata);
	ZVAL_ZVAL(phar_obj->arc.archive->metadata, metadata, 1, 0);

	phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto int Phar::delMetadata()
 * Deletes the global metadata of the phar
 */
PHP_METHOD(Phar, delMetadata)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly) && !phar_obj->arc.archive->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}
	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot delete metadata, not possible with tar-based phar archives");
		return;
	}
	if (phar_obj->arc.archive->metadata) {
		zval_ptr_dtor(&phar_obj->arc.archive->metadata);
		phar_obj->arc.archive->metadata = NULL;

		phar_flush(phar_obj->arc.archive, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		} else {
			RETURN_TRUE;
		}
	} else {
		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ proto void PharFileInfo::__construct(string entry)
 * Construct a Phar entry object
 */
PHP_METHOD(PharFileInfo, __construct)
{
	char *fname, *arch, *entry, *error;
	int fname_len, arch_len, entry_len;
	phar_entry_object  *entry_obj;
	phar_entry_info    *entry_info;
	phar_archive_data *phar_data;
	zval *zobj = getThis(), arg1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}
	
	entry_obj = (phar_entry_object*)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (entry_obj->ent.entry) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot call constructor twice");
		return;
	}

	if (fname_len < 7 || memcmp(fname, "phar://", 7) || phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len, 2, 0 TSRMLS_CC) == FAILURE) {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"'%s' is not a valid phar archive URL (must have at least phar://filename.phar)", fname);
		return;
	}

	if (phar_open_filename(arch, arch_len, NULL, 0, REPORT_ERRORS, &phar_data, &error TSRMLS_CC) == FAILURE) {
		efree(arch);
		efree(entry);
		if (error) {
			zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
				"Cannot open phar file '%s': %s", fname, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
				"Cannot open phar file '%s'", fname);
		}
		return;
	}

	if ((entry_info = phar_get_entry_info_dir(phar_data, entry, entry_len, 1, &error TSRMLS_CC)) == NULL) {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"Cannot access phar file entry '%s' in archive '%s'%s%s", entry, arch, error?", ":"", error?error:"");
		efree(arch);
		efree(entry);
		return;
	}

	efree(arch);
	efree(entry);

	entry_obj->ent.entry = entry_info;

	INIT_PZVAL(&arg1);
	ZVAL_STRINGL(&arg1, fname, fname_len, 0);

	zend_call_method_with_1_params(&zobj, Z_OBJCE_P(zobj), 
		&spl_ce_SplFileInfo->constructor, "__construct", NULL, &arg1);
}
/* }}} */

#define PHAR_ENTRY_OBJECT() \
	phar_entry_object *entry_obj = (phar_entry_object*)zend_object_store_get_object(getThis() TSRMLS_CC); \
	if (!entry_obj->ent.entry) { \
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Cannot call method on an uninitialized PharFileInfo object"); \
		return; \
	}

/* {{{ proto void PharFileInfo::__destruct()
 * clean up directory-based entry objects
 */
PHP_METHOD(PharFileInfo, __destruct)
{
	phar_entry_object *entry_obj = (phar_entry_object*)zend_object_store_get_object(getThis() TSRMLS_CC); \

	if (entry_obj->ent.entry && entry_obj->ent.entry->is_temp_dir) {
		if (entry_obj->ent.entry->filename) {
			efree(entry_obj->ent.entry->filename);
			entry_obj->ent.entry->filename = NULL;
		}
		efree(entry_obj->ent.entry);
		entry_obj->ent.entry = NULL;
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::getCompressedSize()
 * Returns the compressed size
 */
PHP_METHOD(PharFileInfo, getCompressedSize)
{
	PHAR_ENTRY_OBJECT();

	RETURN_LONG(entry_obj->ent.entry->compressed_filesize);
}
/* }}} */

/* {{{ proto bool PharFileInfo::isCompressed([int compression_type])
 * Returns whether the entry is compressed, and whether it is compressed with Phar::GZ or Phar::BZ2 if specified
 */
PHP_METHOD(PharFileInfo, isCompressed)
{
	/* a number that is not Phar::GZ or Phar::BZ2 */
	long method = 9021976;
	PHAR_ENTRY_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &method) == FAILURE) {
		return;
	}

	switch (method) {
		case 9021976:
			RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSION_MASK);
		case PHAR_ENT_COMPRESSED_GZ:
			RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ);
		case PHAR_ENT_COMPRESSED_BZ2:
			RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2);
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
				"Unknown compression type specified"); \
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::getCRC32()
 * Returns CRC32 code or throws an exception if not CRC checked
 */
PHP_METHOD(PharFileInfo, getCRC32)
{
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, does not have a CRC"); \
		return;
	}
	if (entry_obj->ent.entry->is_crc_checked) {
		RETURN_LONG(entry_obj->ent.entry->crc32);
	} else {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry was not CRC checked"); \
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::isCRCChecked()
 * Returns whether file entry is CRC checked
 */
PHP_METHOD(PharFileInfo, isCRCChecked)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->is_crc_checked);
}
/* }}} */

/* {{{ proto int PharFileInfo::getPharFlags()
 * Returns the Phar file entry flags
 */
PHP_METHOD(PharFileInfo, getPharFlags)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_LONG(entry_obj->ent.entry->flags & ~(PHAR_ENT_PERM_MASK|PHAR_ENT_COMPRESSION_MASK));
}
/* }}} */

/* {{{ proto int PharFileInfo::chmod()
 * set the file permissions for the Phar.  This only allows setting execution bit, read/write
 */
PHP_METHOD(PharFileInfo, chmod)
{
	char *error;
	long perms;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_temp_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry \"%s\" is a temporary directory (not an actual entry in the archive), cannot chmod", entry_obj->ent.entry->filename); \
		return;
	}
	if (PHAR_G(readonly) && !entry_obj->ent.entry->phar->is_data) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Cannot modify permissions for file \"%s\" in phar \"%s\", write operations are prohibited", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname);
		return;
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &perms) == FAILURE) {
		return;
	}	
	/* clear permissions */ 
	entry_obj->ent.entry->flags &= ~PHAR_ENT_PERM_MASK;
	perms &= 0777;
	entry_obj->ent.entry->flags |= perms;
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;
	/* hackish cache in php_stat needs to be cleared */
	/* if this code fails to work, check main/streams/streams.c, _php_stream_stat_path */
	if (BG(CurrentLStatFile)) {
		efree(BG(CurrentLStatFile));
	}
	if (BG(CurrentStatFile)) {
		efree(BG(CurrentStatFile));
	}
	BG(CurrentLStatFile) = NULL;
	BG(CurrentStatFile) = NULL;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::hasMetaData()
 * Returns the metadata of the entry
 */
PHP_METHOD(PharFileInfo, hasMetadata)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->metadata != NULL);
}
/* }}} */

/* {{{ proto int PharFileInfo::getMetaData()
 * Returns the metadata of the entry
 */
PHP_METHOD(PharFileInfo, getMetadata)
{
	PHAR_ENTRY_OBJECT();
	
	if (entry_obj->ent.entry->metadata) {
		RETURN_ZVAL(entry_obj->ent.entry->metadata, 1, 0);
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::setMetaData(mixed $metadata)
 * Sets the metadata of the entry
 */
PHP_METHOD(PharFileInfo, setMetadata)
{
	char *error;
	zval *metadata;
	PHAR_ENTRY_OBJECT();

	if (PHAR_G(readonly) && !entry_obj->ent.entry->phar->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by phar.readonly INI setting");
		return;
	}
	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set metadata, not possible with tar-based phar archives");
		return;
	}
	if (entry_obj->ent.entry->is_temp_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a temporary directory (not an actual entry in the archive), cannot set metadata"); \
		return;
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &metadata) == FAILURE) {
		return;
	}

	if (entry_obj->ent.entry->metadata) {
		zval_ptr_dtor(&entry_obj->ent.entry->metadata);
		entry_obj->ent.entry->metadata = NULL;
	}

	MAKE_STD_ZVAL(entry_obj->ent.entry->metadata);
	ZVAL_ZVAL(entry_obj->ent.entry->metadata, metadata, 1, 0);

	phar_flush(entry_obj->ent.entry->phar, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool PharFileInfo::delMetaData()
 * Deletes the metadata of the entry
 */
PHP_METHOD(PharFileInfo, delMetadata)
{
	char *error;
	PHAR_ENTRY_OBJECT();

	if (PHAR_G(readonly) && !entry_obj->ent.entry->phar->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by phar.readonly INI setting");
		return;
	}
	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot delete metadata, not possible with tar-based phar archives");
		return;
	}
	if (entry_obj->ent.entry->is_temp_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a temporary directory (not an actual entry in the archive), cannot delete metadata"); \
		return;
	}
	if (entry_obj->ent.entry->metadata) {
		zval_ptr_dtor(&entry_obj->ent.entry->metadata);
		entry_obj->ent.entry->metadata = NULL;

		phar_flush(entry_obj->ent.entry->phar, 0, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		} else {
			RETURN_TRUE;
		}
	} else {
		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ proto string PharFileInfo::getContent()
 * return the complete file contents of the entry (like file_get_contents)
 */
PHP_METHOD(PharFileInfo, getContent)
{
	char *error;
	php_stream *fp;
	phar_entry_info *link;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar error: Cannot retrieve contents, \"%s\" in phar \"%s\" is a directory", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname);
		return;
	}
	link = phar_get_link_source(entry_obj->ent.entry TSRMLS_CC);
	if (!link) {
		link = entry_obj->ent.entry;
	}
	if (SUCCESS != phar_open_entry_fp(link, &error, 0 TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar error: Cannot retrieve contents, \"%s\" in phar \"%s\": %s", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname, error);
		efree(error);
		return;
	}
	if (!(fp = phar_get_efp(link, 0 TSRMLS_CC))) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar error: Cannot retrieve contents of \"%s\" in phar \"%s\"", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname);
		return;
	}
	phar_seek_efp(link, 0, SEEK_SET, 0, 0 TSRMLS_CC);
	Z_TYPE_P(return_value) = IS_STRING;
	Z_STRLEN_P(return_value) = php_stream_copy_to_mem(fp, &(Z_STRVAL_P(return_value)), link->uncompressed_filesize, 0);
	if (!Z_STRVAL_P(return_value)) {
		Z_STRVAL_P(return_value) = estrndup("", 0);
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::compress(int compression_type)
 * Instructs the Phar class to compress the current file using zlib or bzip2 compression
 */
PHP_METHOD(PharFileInfo, compress)
{
	long method = 9021976;
	char *error;
	PHAR_ENTRY_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &method) == FAILURE) {
		return;
	}

	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Gzip compression, not possible with tar-based phar archives");
		return;
	}
	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set compression"); \
		return;
	}
	if (PHAR_G(readonly) && !entry_obj->ent.entry->phar->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
		return;
	}
	if (entry_obj->ent.entry->is_deleted) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress deleted file");
		return;
	}

	switch (method) {
		case PHAR_ENT_COMPRESSED_GZ:
			if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ) {
				RETURN_TRUE;
				return;
			}
			if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2) != 0) {
				if (!phar_has_bz2) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
						"Cannot compress with gzip compression, file is already compressed with bzip2 compression and bz2 extension is not enabled, cannot decompress");
					return;
				}
				/* decompress this file indirectly */
				if (SUCCESS != phar_open_entry_fp(entry_obj->ent.entry, &error, 1 TSRMLS_CC)) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
						"Phar error: Cannot decompress bzip2-compressed file \"%s\" in phar \"%s\" in order to compress with gzip: %s", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname, error);
					efree(error);
					return;
				}
			}
			if (!phar_has_zlib) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress with gzip compression, zlib extension is not enabled");
				return;
			}
			entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
			entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
			entry_obj->ent.entry->flags |= PHAR_ENT_COMPRESSED_GZ;
			break;
		case PHAR_ENT_COMPRESSED_BZ2:
			if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
				RETURN_TRUE;
				return;
			}
			if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ) != 0) {
				if (!phar_has_zlib) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
						"Cannot compress with bzip2 compression, file is already compressed with gzip compression and zlib extension is not enabled, cannot decompress");
					return;
				}
				/* decompress this file indirectly */
				if (SUCCESS != phar_open_entry_fp(entry_obj->ent.entry, &error, 1 TSRMLS_CC)) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
						"Phar error: Cannot decompress gzip-compressed file \"%s\" in phar \"%s\" in order to compress with bzip2: %s", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname, error);
					efree(error);
					return;
				}
			}
			if (!phar_has_bz2) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
					"Cannot compress with bzip2 compression, bz2 extension is not enabled");
				return;
			}
			entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
			entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
			entry_obj->ent.entry->flags |= PHAR_ENT_COMPRESSED_BZ2;
			break;
		default:
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
				"Unknown compression type specified"); \
	}

	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int PharFileInfo::decompress()
 * Instructs the Phar class to decompress the current file
 */
PHP_METHOD(PharFileInfo, decompress)
{
	char *error;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set compression"); \
		return;
	}
	if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSION_MASK) == 0) {
		RETURN_TRUE;
		return;
	}
	if (PHAR_G(readonly) && !entry_obj->ent.entry->phar->is_data) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot decompress");
		return;
	}
	if (entry_obj->ent.entry->is_deleted) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress deleted file");
		return;
	}
	if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ) != 0 && !phar_has_zlib) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot decompress Gzip-compressed file, zlib extension is not enabled");
		return;
	}
	if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2) != 0 && !phar_has_bz2) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot decompress Bzip2-compressed file, bz2 extension is not enabled");
		return;
	}
	if (!entry_obj->ent.entry->fp) {
		if (FAILURE == phar_open_archive_fp(entry_obj->ent.entry->phar TSRMLS_CC)) {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot decompress entry \"%s\", phar error: Cannot open phar archive \"%s\" for reading", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname);
			return;
		}
		entry_obj->ent.entry->fp_type = PHAR_FP;
	}
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
}
/* }}} */

#endif /* HAVE_SPL */

/* {{{ phar methods */

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, alias)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_createDS, 0, 0, 0)
	ZEND_ARG_INFO(0, index)
	ZEND_ARG_INFO(0, webindex)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_loadPhar, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, alias)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_mapPhar, 0, 0, 0)
	ZEND_ARG_INFO(0, alias)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_mount, 0, 0, 2)
	ZEND_ARG_INFO(0, inphar)
	ZEND_ARG_INFO(0, externalfile)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_mungServer, 0, 0, 1)
	ZEND_ARG_INFO(0, munglist)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_webPhar, 0, 0, 0)
	ZEND_ARG_INFO(0, alias)
	ZEND_ARG_INFO(0, index)
	ZEND_ARG_INFO(0, f404)
	ZEND_ARG_INFO(0, mimetypes)
	ZEND_ARG_INFO(0, rewrites)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_running, 0, 0, 1)
	ZEND_ARG_INFO(0, retphar)
ZEND_END_ARG_INFO();

#if HAVE_SPL

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_build, 0, 0, 1)
	ZEND_ARG_INFO(0, iterator)
	ZEND_ARG_INFO(0, base_directory)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_conv, 0, 0, 0)
	ZEND_ARG_INFO(0, format)
	ZEND_ARG_INFO(0, compression_type)
	ZEND_ARG_INFO(0, file_ext)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_comps, 0, 0, 1)
	ZEND_ARG_INFO(0, compression_type)
	ZEND_ARG_INFO(0, file_ext)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_decomp, 0, 0, 0)
	ZEND_ARG_INFO(0, file_ext)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_comp, 0, 0, 1)
	ZEND_ARG_INFO(0, compression_type)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_compo, 0, 0, 0)
	ZEND_ARG_INFO(0, compression_type)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_copy, 0, 0, 2)
	ZEND_ARG_INFO(0, newfile)
	ZEND_ARG_INFO(0, oldfile)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, entry)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_fromdir, 0, 0, 1)
	ZEND_ARG_INFO(0, dir_path)
	ZEND_ARG_INFO(0, regex)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_offsetExists, 0, 0, 1)
	ZEND_ARG_INFO(0, entry)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_offsetSet, 0, 0, 2)
	ZEND_ARG_INFO(0, entry)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_setAlias, 0, 0, 1)
	ZEND_ARG_INFO(0, alias)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_setMetadata, 0, 0, 1)
	ZEND_ARG_INFO(0, metadata)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_setSigAlgo, 0, 0, 1)
	ZEND_ARG_INFO(0, algorithm)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_setStub, 0, 0, 1)
	ZEND_ARG_INFO(0, newstub)
	ZEND_ARG_INFO(0, maxlen)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_emptydir, 0, 0, 0)
	ZEND_ARG_INFO(0, dirname)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_addfile, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, localname)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_fromstring, 0, 0, 1)
	ZEND_ARG_INFO(0, localname)
	ZEND_ARG_INFO(0, contents)
ZEND_END_ARG_INFO();

#endif /* HAVE_SPL */

zend_function_entry php_archive_methods[] = {
#if !HAVE_SPL
	PHP_ME(Phar, __construct,           arginfo_phar___construct,  ZEND_ACC_PRIVATE)
#else
	PHP_ME(Phar, __construct,           arginfo_phar___construct,  ZEND_ACC_PUBLIC)
	PHP_ME(Phar, addEmptyDir,           arginfo_phar_emptydir,     ZEND_ACC_PUBLIC)
	PHP_ME(Phar, addFile,               arginfo_phar_addfile,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, addFromString,         arginfo_phar_fromstring,   ZEND_ACC_PUBLIC)
	PHP_ME(Phar, buildFromDirectory,    arginfo_phar_fromdir,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, buildFromIterator,     arginfo_phar_build,        ZEND_ACC_PUBLIC)
	PHP_ME(Phar, compressFiles,         arginfo_phar_comp,         ZEND_ACC_PUBLIC)
	PHP_ME(Phar, decompressFiles,       NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, compress,              arginfo_phar_comps,        ZEND_ACC_PUBLIC)
	PHP_ME(Phar, decompress,            arginfo_phar_decomp,       ZEND_ACC_PUBLIC)
	PHP_ME(Phar, convertToExecutable,   arginfo_phar_conv,         ZEND_ACC_PUBLIC)
	PHP_ME(Phar, convertToData,         arginfo_phar_conv,         ZEND_ACC_PUBLIC)
	PHP_ME(Phar, copy,                  arginfo_phar_copy,         ZEND_ACC_PUBLIC)
	PHP_ME(Phar, count,                 NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, delete,                arginfo_phar_delete,       ZEND_ACC_PUBLIC)
	PHP_ME(Phar, delMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getAlias,              NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getPath,               NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getModified,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getSignature,          NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getStub,               NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getVersion,            NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, hasMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isBuffering,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isCompressed,          NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isWritable,            NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isPhar,                NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isTar,                 NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isZip,                 NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetExists,          arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetGet,             arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetSet,             arginfo_phar_offsetSet,    ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetUnset,           arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setAlias,              arginfo_phar_setAlias,     ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setDefaultStub,        arginfo_phar_createDS,     ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setMetadata,           arginfo_phar_setMetadata,  ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setSignatureAlgorithm, arginfo_phar_setSigAlgo,   ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setStub,               arginfo_phar_setStub,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, startBuffering,        NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, stopBuffering,         NULL,                      ZEND_ACC_PUBLIC)
#endif
	/* static member functions */
	PHP_ME(Phar, apiVersion,            NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, canCompress,           NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, canWrite,              NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, createDefaultStub,     arginfo_phar_createDS,     ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getExtractList,        NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getSupportedCompression,NULL,                     ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getSupportedSignatures,NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, interceptFileFuncs,    NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, isValidPharFilename,   NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, loadPhar,              arginfo_phar_loadPhar,     ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, mapPhar,               arginfo_phar_mapPhar,      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, running,               arginfo_phar_running,      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, mount,                 arginfo_phar_mount,        ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, mungServer,            arginfo_phar_mungServer,   ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, webPhar,               arginfo_phar_webPhar,      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	{NULL, NULL, NULL}
};

#if HAVE_SPL
static
ZEND_BEGIN_ARG_INFO_EX(arginfo_entry___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_entry_chmod, 0, 0, 1)
	ZEND_ARG_INFO(0, perms)
ZEND_END_ARG_INFO();

zend_function_entry php_entry_methods[] = {
	PHP_ME(PharFileInfo, __construct,        arginfo_entry___construct,  ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, __destruct,         NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, chmod,              arginfo_entry_chmod,        ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, delMetadata,        NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, getContent,         NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, getCompressedSize,  NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, getCRC32,           NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, getMetadata,        NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, getPharFlags,       NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, hasMetadata,        NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, isCompressed,       arginfo_phar_compo,         ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, isCRCChecked,       NULL,                       ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, compress,           arginfo_phar_comp,          ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, setMetadata,        arginfo_phar_setMetadata,   ZEND_ACC_PUBLIC)
	PHP_ME(PharFileInfo, decompress,         NULL,                       ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
#endif /* HAVE_SPL */

zend_function_entry phar_exception_methods[] = {
	{NULL, NULL, NULL}
};
/* }}} */

#define REGISTER_PHAR_CLASS_CONST_LONG(class_name, const_name, value) \
	zend_declare_class_constant_long(class_name, const_name, sizeof(const_name)-1, (long)value TSRMLS_CC);

#if PHP_VERSION_ID < 50200
# define phar_exception_get_default() zend_exception_get_default()
#else
# define phar_exception_get_default() zend_exception_get_default(TSRMLS_C)
#endif

void phar_object_init(TSRMLS_D) /* {{{ */
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "PharException", phar_exception_methods);
	phar_ce_PharException = zend_register_internal_class_ex(&ce, phar_exception_get_default(), NULL  TSRMLS_CC);

#if HAVE_SPL
	INIT_CLASS_ENTRY(ce, "Phar", php_archive_methods);
	phar_ce_archive = zend_register_internal_class_ex(&ce, spl_ce_RecursiveDirectoryIterator, NULL  TSRMLS_CC);

	zend_class_implements(phar_ce_archive TSRMLS_CC, 2, spl_ce_Countable, zend_ce_arrayaccess);

	INIT_CLASS_ENTRY(ce, "PharData", php_archive_methods);
	phar_ce_data = zend_register_internal_class_ex(&ce, spl_ce_RecursiveDirectoryIterator, NULL  TSRMLS_CC);

	zend_class_implements(phar_ce_data TSRMLS_CC, 2, spl_ce_Countable, zend_ce_arrayaccess);

	INIT_CLASS_ENTRY(ce, "PharFileInfo", php_entry_methods);
	phar_ce_entry = zend_register_internal_class_ex(&ce, spl_ce_SplFileInfo, NULL  TSRMLS_CC);
#else
	INIT_CLASS_ENTRY(ce, "Phar", php_archive_methods);
	phar_ce_archive = zend_register_internal_class(&ce TSRMLS_CC);
	phar_ce_archive->ce_flags |= ZEND_ACC_FINAL_CLASS;

	INIT_CLASS_ENTRY(ce, "PharData", php_archive_methods);
	phar_ce_data = zend_register_internal_class(&ce TSRMLS_CC);
	phar_ce_data->ce_flags |= ZEND_ACC_FINAL_CLASS;
#endif

	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "BZ2", PHAR_ENT_COMPRESSED_BZ2)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "GZ", PHAR_ENT_COMPRESSED_GZ)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "NONE", PHAR_ENT_COMPRESSED_NONE)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PHAR", PHAR_FORMAT_PHAR)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "TAR", PHAR_FORMAT_TAR)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "ZIP", PHAR_FORMAT_ZIP)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "COMPRESSED", PHAR_ENT_COMPRESSION_MASK)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PHP", PHAR_MIME_PHP)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PHPS", PHAR_MIME_PHPS)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "MD5", PHAR_SIG_MD5)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PGP", PHAR_SIG_PGP)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA1", PHAR_SIG_SHA1)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA256", PHAR_SIG_SHA256)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA512", PHAR_SIG_SHA512)
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
