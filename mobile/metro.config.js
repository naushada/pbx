const path = require('path');
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');

/**
 * Metro bundler configuration.
 * https://reactnative.dev/docs/metro
 *
 * `watchFolders` lets Metro reach into `../shared/` for the sip-ua
 * module shared with the Angular web softphone (ui/).
 */
const sharedRoot = path.resolve(__dirname, '../shared');

module.exports = mergeConfig(getDefaultConfig(__dirname), {
  watchFolders: [sharedRoot],
});
